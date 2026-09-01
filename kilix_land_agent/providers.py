"""Untrusted proposal providers for the resident loop."""

from __future__ import annotations

from dataclasses import dataclass
import json
import re
import subprocess
from typing import Any, Protocol


SAFE_HOST = re.compile(r"[A-Za-z0-9_.:@-]{1,255}\Z")
SAFE_MODEL = re.compile(r"[A-Za-z0-9_.:/-]{1,64}\Z")

TOOLS: list[dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "observe_room",
            "description": "Return authoritative current room state and target IDs.",
            "parameters": {
                "type": "object",
                "properties": {},
                "additionalProperties": False,
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "go_to",
            "description": "Walk Kilix to one ID from the latest observation.",
            "parameters": {
                "type": "object",
                "properties": {"target_id": {"type": "string"}},
                "required": ["target_id"],
                "additionalProperties": False,
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "interact",
            "description": "Use the named entity after Kilix has arrived there.",
            "parameters": {
                "type": "object",
                "properties": {"target_id": {"type": "string"}},
                "required": ["target_id"],
                "additionalProperties": False,
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "face_user",
            "description": "Turn Kilix toward the user before speaking.",
            "parameters": {
                "type": "object",
                "properties": {},
                "additionalProperties": False,
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "say",
            "description": "Speak a short printable-ASCII line as Kilix.",
            "parameters": {
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "minLength": 3,
                        "maxLength": 512,
                        "pattern": "^[ -~]+$",
                    }
                },
                "required": ["text"],
                "additionalProperties": False,
            },
        },
    },
]


class ProviderError(RuntimeError):
    """A proposal provider failed or returned an invalid shape."""


@dataclass(frozen=True)
class Proposal:
    name: str
    arguments: dict[str, Any]
    message: dict[str, Any]
    metrics: dict[str, Any]


class Provider(Protocol):
    label: str

    def propose(self, messages: list[dict[str, Any]]) -> Proposal:
        """Return exactly one untrusted typed proposal."""


def safe_error_text(value: object) -> str:
    text = str(value)
    return "".join(
        character if character.isascii() and 32 <= ord(character) <= 126 else "?"
        for character in text[:200]
    )


class DeterministicProvider:
    """Reproducible provider used by CI and the first live-loop proof."""

    label = "deterministic/replay"

    def __init__(self, *, target_id: str, speech: str):
        self._steps = [
            ("observe_room", {}),
            ("go_to", {"target_id": target_id}),
            ("interact", {"target_id": target_id}),
            ("face_user", {}),
            ("say", {"text": speech}),
        ]
        self._index = 0

    def propose(self, messages: list[dict[str, Any]]) -> Proposal:
        del messages
        if self._index >= len(self._steps):
            raise ProviderError("the deterministic plan is complete")
        name, arguments = self._steps[self._index]
        self._index += 1
        message = {
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {"function": {"name": name, "arguments": arguments}}
            ],
        }
        return Proposal(name=name, arguments=dict(arguments), message=message,
                        metrics={"deterministic": True})


class OllamaSshProvider:
    """Qwen/Ollama adapter whose only network destination is an approved SSH host."""

    def __init__(self, *, host: str, model: str):
        if not SAFE_HOST.fullmatch(host) or host.startswith("-"):
            raise ProviderError("SSH host contains unsupported characters")
        if (not SAFE_MODEL.fullmatch(model) or model.startswith("-") or
                ".." in model):
            raise ProviderError("model ID contains unsupported characters")
        self.host = host
        self.model = model
        self.label = f"ollama-ssh/{model}"

    @staticmethod
    def _metrics(response: dict[str, Any]) -> dict[str, Any]:
        count = response.get("eval_count")
        duration = response.get("eval_duration")
        rate = None
        if (isinstance(count, int) and not isinstance(count, bool) and
                isinstance(duration, int) and not isinstance(duration, bool) and
                duration > 0):
            rate = round(count / (duration / 1_000_000_000), 2)
        return {
            "prompt_tokens": response.get("prompt_eval_count"),
            "output_tokens": count,
            "output_tokens_per_second": rate,
            "total_duration_ns": response.get("total_duration"),
        }

    def propose(self, messages: list[dict[str, Any]]) -> Proposal:
        payload = {
            "model": self.model,
            "messages": messages,
            "tools": TOOLS,
            "stream": False,
            "think": False,
            "keep_alive": "5m",
            "options": {
                "temperature": 0,
                "num_ctx": 8192,
                "num_predict": 160,
                "seed": 7,
            },
        }
        command = [
            "ssh",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            self.host,
            "curl",
            "--fail",
            "--silent",
            "--show-error",
            "--max-time", "180",
            "-H", "Content-Type:application/json",
            "--data-binary", "@-",
            "http://127.0.0.1:11434/api/chat",
        ]
        try:
            result = subprocess.run(
                command,
                input=json.dumps(payload, separators=(",", ":")),
                check=False,
                capture_output=True,
                text=True,
                timeout=195,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise ProviderError("the Ollama SSH transport failed") from error
        if result.returncode != 0:
            detail = result.stderr.strip().splitlines()
            suffix = f": {safe_error_text(detail[-1])}" if detail else ""
            raise ProviderError(f"Ollama request failed{suffix}")
        try:
            response = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise ProviderError("Ollama returned invalid JSON") from error
        if not isinstance(response, dict):
            raise ProviderError("Ollama returned a non-object response")
        if response.get("error"):
            raise ProviderError(
                f"Ollama rejected the request: {safe_error_text(response['error'])}"
            )
        message = response.get("message")
        calls = message.get("tool_calls") if isinstance(message, dict) else None
        if not isinstance(calls, list) or len(calls) != 1:
            raise ProviderError("the model must propose exactly one tool")
        call = calls[0]
        function = call.get("function") if isinstance(call, dict) else None
        if not isinstance(function, dict):
            raise ProviderError("the model returned a malformed tool proposal")
        name = function.get("name")
        if name not in {tool["function"]["name"] for tool in TOOLS}:
            raise ProviderError("the model proposed an unavailable tool")
        arguments = function.get("arguments", {})
        if isinstance(arguments, str):
            try:
                arguments = json.loads(arguments)
            except json.JSONDecodeError as error:
                raise ProviderError("the model returned invalid tool arguments") from error
        if not isinstance(arguments, dict):
            raise ProviderError("the model tool arguments are not an object")
        return Proposal(name=name, arguments=arguments, message=message,
                        metrics=self._metrics(response))
