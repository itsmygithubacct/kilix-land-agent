#!/usr/bin/env python3
"""Ask a remote Ollama model for typed room actions, then replay them visibly."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
ROOM_BINARY = PROJECT_ROOT / "kilix-land-agent"
TRANSCRIPT_PATH = PROJECT_ROOT / "build" / "qwen-demo-transcript.json"
DEFAULT_REQUEST = "Walk Kilix to the bookshelf and inspect it."
SAFE_HOST = re.compile(r"[A-Za-z0-9_.:@-]{1,255}\Z")
SAFE_MODEL = re.compile(r"[A-Za-z0-9_.:/-]{1,31}\Z")

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "observe_room",
            "description": (
                "Return authoritative room state and the semantic IDs Kilix "
                "may currently target."
            ),
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
            "description": (
                "Walk Kilix to one entity ID from the latest observation."
            ),
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
            "description": "Use the nearby entity after Kilix has arrived.",
            "parameters": {
                "type": "object",
                "properties": {"target_id": {"type": "string"}},
                "required": ["target_id"],
                "additionalProperties": False,
            },
        },
    },
]


class DemoError(RuntimeError):
    """A fail-closed model, transport, or validation error."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a three-tool Qwen room preflight and visibly replay only its "
            "validated semantic target."
        )
    )
    parser.add_argument(
        "--ssh-host",
        required=True,
        help="SSH host whose loopback Ollama service is explicitly authorized",
    )
    parser.add_argument("--model", default="qwen3.5:9b")
    parser.add_argument("--request", default=DEFAULT_REQUEST)
    parser.add_argument(
        "--no-launch",
        action="store_true",
        help="validate and save the transcript without opening the room",
    )
    return parser.parse_args()


def validate_options(args: argparse.Namespace) -> None:
    if not SAFE_HOST.fullmatch(args.ssh_host) or args.ssh_host.startswith("-"):
        raise DemoError("SSH host contains unsupported characters")
    if not SAFE_MODEL.fullmatch(args.model) or args.model.startswith("-"):
        raise DemoError("model ID is not safe for the visual replay label")
    if not args.request.strip() or len(args.request) > 500:
        raise DemoError("request must contain 1 to 500 characters")
    if (
        ROOM_BINARY.is_symlink()
        or not ROOM_BINARY.is_file()
        or not os.access(ROOM_BINARY, os.X_OK)
    ):
        raise DemoError("build kilix-land-agent before running the demo")


def safe_error_text(value: object) -> str:
    text = str(value)
    return "".join(
        character if 32 <= ord(character) <= 126 else "?"
        for character in text[:200]
    )


def room_observation() -> dict[str, Any]:
    result = subprocess.run(
        [str(ROOM_BINARY), "--observe"],
        cwd=PROJECT_ROOT,
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode != 0:
        raise DemoError("trusted room observation failed")
    try:
        observation = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise DemoError("trusted room returned invalid JSON") from error
    if observation.get("protocol") != "kilix.land.observe/v1":
        raise DemoError("trusted room returned an unsupported protocol")
    return observation


def ollama_chat(host: str, payload: dict[str, Any]) -> dict[str, Any]:
    command = [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        host,
        "curl",
        "--fail",
        "--silent",
        "--show-error",
        "--max-time",
        "180",
        "-H",
        "Content-Type:application/json",
        "--data-binary",
        "@-",
        "http://127.0.0.1:11434/api/chat",
    ]
    result = subprocess.run(
        command,
        input=json.dumps(payload, separators=(",", ":")),
        check=False,
        capture_output=True,
        text=True,
        timeout=195,
    )
    if result.returncode != 0:
        detail = result.stderr.strip().splitlines()
        suffix = f": {safe_error_text(detail[-1])}" if detail else ""
        raise DemoError(f"Ollama request failed{suffix}")
    try:
        response = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise DemoError("Ollama returned invalid JSON") from error
    if response.get("error"):
        raise DemoError(
            f"Ollama rejected the request: {safe_error_text(response['error'])}"
        )
    return response


def tool_call(response: dict[str, Any], expected: str) -> tuple[dict[str, Any], dict[str, Any]]:
    message = response.get("message")
    calls = message.get("tool_calls") if isinstance(message, dict) else None
    if not isinstance(calls, list) or len(calls) != 1:
        raise DemoError(f"expected exactly one {expected} proposal")
    function = calls[0].get("function")
    if not isinstance(function, dict) or function.get("name") != expected:
        proposed = function.get("name") if isinstance(function, dict) else None
        raise DemoError(f"expected {expected}, received {proposed!r}")
    arguments = function.get("arguments", {})
    if isinstance(arguments, str):
        try:
            arguments = json.loads(arguments)
        except json.JSONDecodeError as error:
            raise DemoError(f"{expected} arguments are invalid JSON") from error
    if not isinstance(arguments, dict):
        raise DemoError(f"{expected} arguments are not an object")
    return message, arguments


def metrics(response: dict[str, Any]) -> dict[str, Any]:
    count = response.get("eval_count")
    duration = response.get("eval_duration")
    rate = None
    if isinstance(count, int) and isinstance(duration, int) and duration > 0:
        rate = round(count / (duration / 1_000_000_000), 2)
    return {
        "prompt_tokens": response.get("prompt_eval_count"),
        "output_tokens": count,
        "output_tokens_per_second": rate,
        "total_duration_ns": response.get("total_duration"),
    }


def request_payload(model: str, messages: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "model": model,
        "messages": messages,
        "tools": TOOLS,
        "stream": False,
        "think": False,
        "keep_alive": "5m",
        "options": {
            "temperature": 0,
            "num_ctx": 8192,
            "num_predict": 128,
            "seed": 7,
        },
    }


def atomic_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.parent.is_symlink() or path.is_symlink():
        raise DemoError("refusing a symlinked transcript path")
    project = PROJECT_ROOT.resolve(strict=True)
    parent = path.parent.resolve(strict=True)
    if os.path.commonpath((project, parent)) != str(project):
        raise DemoError("transcript path escapes the project workspace")
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(document, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def run_preflight(args: argparse.Namespace) -> str:
    observation = room_observation()
    entities = observation.get("entities")
    if not isinstance(entities, list):
        raise DemoError("room observation has no entity catalog")
    target_ids = {
        item.get("entity_id")
        for item in entities
        if isinstance(item, dict) and isinstance(item.get("entity_id"), str)
    }
    if not target_ids:
        raise DemoError("room observation contains no semantic targets")

    messages: list[dict[str, Any]] = [
        {
            "role": "system",
            "content": (
                "You control Kilix only through typed tools. Call exactly one "
                "tool per turn. Before a physical action, call observe_room. "
                "After reaching the requested target, call interact. Use only "
                "entity IDs returned by the latest observation."
            ),
        },
        {"role": "user", "content": args.request.strip()},
    ]
    proposals: list[dict[str, Any]] = []

    response = ollama_chat(args.ssh_host, request_payload(args.model, messages))
    message, arguments = tool_call(response, "observe_room")
    if arguments:
        raise DemoError("observe_room proposed unexpected arguments")
    proposals.append({"tool": "observe_room", "arguments": {}, "metrics": metrics(response)})
    print(f"{args.model}: observe_room() -> validated", flush=True)
    messages.extend(
        [message, {"role": "tool", "tool_name": "observe_room", "content": json.dumps(observation, separators=(",", ":"))}]
    )

    response = ollama_chat(args.ssh_host, request_payload(args.model, messages))
    message, arguments = tool_call(response, "go_to")
    if set(arguments) != {"target_id"} or arguments["target_id"] not in target_ids:
        raise DemoError("go_to proposed an unknown target or extra arguments")
    target_id = arguments["target_id"]
    proposals.append({"tool": "go_to", "arguments": arguments, "metrics": metrics(response)})
    print(f"{args.model}: go_to({target_id}) -> validated", flush=True)
    messages.extend(
        [
            message,
            {
                "role": "tool",
                "tool_name": "go_to",
                "content": json.dumps(
                    {
                        "protocol": "kilix.land.action-result/v1",
                        "status": "arrived",
                        "target_id": target_id,
                        "preflight_fixture": True,
                    },
                    separators=(",", ":"),
                ),
            },
        ]
    )

    response = ollama_chat(args.ssh_host, request_payload(args.model, messages))
    _message, arguments = tool_call(response, "interact")
    if set(arguments) != {"target_id"} or arguments["target_id"] != target_id:
        raise DemoError("interact did not preserve the validated nearby target")
    proposals.append({"tool": "interact", "arguments": arguments, "metrics": metrics(response)})
    print(f"{args.model}: interact({target_id}) -> validated", flush=True)

    atomic_json(
        TRANSCRIPT_PATH,
        {
            "protocol": "kilix.agent.demo-transcript/v1",
            "mode": "preflight-validated-visual-replay",
            "model": args.model,
            "request": args.request.strip(),
            "observation_protocol": observation.get("protocol"),
            "room_id": observation.get("room_id"),
            "selected_target": target_id,
            "proposals": proposals,
        },
    )
    print(f"Transcript: {TRANSCRIPT_PATH}", flush=True)
    return target_id


def main() -> int:
    args = parse_args()
    try:
        validate_options(args)
        target_id = run_preflight(args)
    except (DemoError, OSError, subprocess.SubprocessError) as error:
        print(f"qwen-room-demo: {error}", file=sys.stderr)
        return 1
    if args.no_launch:
        return 0
    os.execv(
        ROOM_BINARY,
        [str(ROOM_BINARY), "--agent-replay", args.model, target_id],
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
