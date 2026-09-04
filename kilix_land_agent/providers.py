"""Untrusted proposal providers for the resident loop."""

from __future__ import annotations

from dataclasses import dataclass
import json
import re
import subprocess
import time
from typing import Any, Callable, Protocol


SAFE_HOST = re.compile(r"[A-Za-z0-9_.:@-]{1,255}\Z")
SAFE_MODEL = re.compile(r"[A-Za-z0-9_.:/-]{1,64}\Z")
SSH_BINARY = "/usr/bin/ssh"
REMOTE_CURL_BINARY = "/usr/bin/curl"

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
    {
        "type": "function",
        "function": {
            "name": "search_help",
            "description": (
                "Search read-only Kilix help documents under the trusted "
                "development or installed gpu_terminal root. Space-separated "
                "terms are matched across each relative path and line."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "minLength": 1, "maxLength": 80}
                },
                "required": ["query"],
                "additionalProperties": False,
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "read_help",
            "description": (
                "Read a bounded excerpt from a relative help-document path "
                "returned by search_help."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "minLength": 1, "maxLength": 512},
                    "line_start": {"type": "integer", "minimum": 1},
                },
                "required": ["path", "line_start"],
                "additionalProperties": False,
            },
        },
    },
]


class ProviderError(RuntimeError):
    """A proposal provider failed or returned an invalid shape."""


class _ToolSerializationError(ProviderError):
    """Ollama rejected Qwen's internal tool-call serialization."""


@dataclass(frozen=True)
class Proposal:
    name: str
    arguments: dict[str, Any]
    message: dict[str, Any]
    metrics: dict[str, Any]


@dataclass(frozen=True)
class FinalReply:
    text: str
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
        self._cancel_check: Callable[[], bool] | None = None

    def set_cancel_check(self, callback: Callable[[], bool]) -> None:
        self._cancel_check = callback

    def _canceled(self) -> bool:
        if self._cancel_check is None:
            return False
        try:
            return bool(self._cancel_check())
        except Exception:
            return True

    @staticmethod
    def _stop_process(process: subprocess.Popen[str]) -> None:
        if process.poll() is not None:
            return
        process.terminate()
        try:
            process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()

    def _run_transport(
        self, command: list[str], payload: str
    ) -> subprocess.CompletedProcess[str]:
        if self._cancel_check is None:
            return subprocess.run(
                command,
                input=payload,
                check=False,
                capture_output=True,
                text=True,
                timeout=195,
            )
        if self._canceled():
            raise ProviderError("the room closed before the model request")
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        except OSError as error:
            raise ProviderError("the Ollama SSH transport failed") from error
        deadline = time.monotonic() + 195.0
        pending_input: str | None = payload
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self._stop_process(process)
                raise ProviderError("the Ollama SSH transport timed out")
            try:
                stdout, stderr = process.communicate(
                    input=pending_input, timeout=min(0.25, remaining)
                )
                return subprocess.CompletedProcess(
                    command,
                    process.returncode if process.returncode is not None else -1,
                    stdout,
                    stderr,
                )
            except subprocess.TimeoutExpired:
                pending_input = None
                if self._canceled():
                    self._stop_process(process)
                    raise ProviderError(
                        "the room closed while waiting for the model"
                    )

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

    def _request(
        self, messages: list[dict[str, Any]], *, corrective: bool,
        include_tools: bool = True,
    ) -> dict[str, Any]:
        payload = {
            "model": self.model,
            "messages": messages,
            "stream": False,
            "think": False,
            "keep_alive": "5m",
            "options": {
                "temperature": 0.2 if corrective else 0,
                "num_ctx": 8192,
                "num_predict": 160 if include_tools else 220,
                "seed": 17 if corrective else 7,
            },
        }
        if include_tools:
            payload["tools"] = TOOLS
        command = [
            SSH_BINARY,
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            "--",
            self.host,
            REMOTE_CURL_BINARY,
            "--fail-with-body",
            "--silent",
            "--show-error",
            "--max-time", "180",
            "-H", "Content-Type:application/json",
            "--data-binary", "@-",
            "http://127.0.0.1:11434/api/chat",
        ]
        try:
            result = self._run_transport(
                command, json.dumps(payload, separators=(",", ":"))
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise ProviderError("the Ollama SSH transport failed") from error
        response: Any = None
        if result.stdout.strip():
            try:
                response = json.loads(result.stdout)
            except json.JSONDecodeError:
                response = None
        if result.returncode != 0:
            if isinstance(response, dict) and response.get("error"):
                error_text = str(response["error"])
                error_type = (
                    _ToolSerializationError
                    if "XML syntax error" in error_text
                    else ProviderError
                )
                raise error_type(
                    "Ollama rejected the request: "
                    f"{safe_error_text(error_text)}"
                )
            detail = result.stderr.strip().splitlines()
            suffix = f": {safe_error_text(detail[-1])}" if detail else ""
            raise ProviderError(f"Ollama request failed{suffix}")
        if response is None:
            raise ProviderError("Ollama returned invalid JSON")
        if not isinstance(response, dict):
            raise ProviderError("Ollama returned a non-object response")
        if response.get("error"):
            error_text = str(response["error"])
            error_type = (
                _ToolSerializationError
                if "XML syntax error" in error_text
                else ProviderError
            )
            raise error_type(
                f"Ollama rejected the request: {safe_error_text(error_text)}"
            )
        return response

    def propose(self, messages: list[dict[str, Any]]) -> Proposal:
        try:
            response = self._request(messages, corrective=False)
        except _ToolSerializationError:
            corrected_messages = [dict(message) for message in messages]
            corrected_messages.append(
                {
                    "role": "system",
                    "content": (
                        "Your previous tool serialization was invalid. Emit "
                        "exactly one valid supplied tool call with a JSON "
                        "object for its arguments."
                    ),
                }
            )
            response = self._request(corrected_messages, corrective=True)
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

    def final_reply(self, messages: list[dict[str, Any]]) -> FinalReply:
        final_messages = [dict(message) for message in messages]
        final_messages.append(
            {
                "role": "system",
                "content": (
                    "Now answer the user as Kilix in one concise plain-text "
                    "sentence. Use printable ASCII only. Do not emit XML, "
                    "Markdown, analysis, or a tool call."
                ),
            }
        )
        response = self._request(
            final_messages, corrective=False, include_tools=False
        )
        message = response.get("message")
        content = message.get("content") if isinstance(message, dict) else None
        if not isinstance(content, str) or not content.strip():
            raise ProviderError("the model returned an empty final reply")
        return FinalReply(
            text=content,
            message=message,
            metrics=self._metrics(response),
        )
