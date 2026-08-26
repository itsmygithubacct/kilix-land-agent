#!/usr/bin/env python3
"""Create a 30-second Qwen-driven Kilix room demo with local TTS audio."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any

from qwen_room_demo import (
    DemoError,
    PROJECT_ROOT,
    ROOM_BINARY,
    SAFE_HOST,
    SAFE_MODEL,
    atomic_json,
    metrics,
    ollama_chat,
    room_observation,
    safe_error_text,
    tool_call,
)


OUTPUT_PATH = PROJECT_ROOT / "assets" / "demo" / "qwen-kilix-studio-demo-30s.mp4"
TRANSCRIPT_PATH = PROJECT_ROOT / "build" / "qwen-video-transcript.json"
REQUEST = (
    "Move Kilix around the studio. Visit and inspect the computer, then visit "
    "and inspect the bookshelf. Face the user and say one warm, playful ASCII "
    "sentence of 3 to 9 words introducing the room."
)

VIDEO_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "observe_room",
            "description": "Return authoritative room state and semantic entity IDs.",
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
            "description": "Walk Kilix to an entity ID from the latest observation.",
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
            "description": "Inspect the nearby entity after Kilix arrives.",
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
            "description": "Turn Kilix to face the user before speaking.",
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
            "description": "Speak one short printable-ASCII sentence as Kilix.",
            "parameters": {
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "minLength": 3,
                        "maxLength": 63,
                        "pattern": "^[ -~]+$",
                    }
                },
                "required": ["text"],
                "additionalProperties": False,
            },
        },
    },
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the fixed 30-second Qwen/Kilix MP4 demo."
    )
    parser.add_argument(
        "--ssh-host",
        required=True,
        help="approved SSH host whose loopback Ollama service will be used",
    )
    parser.add_argument("--model", default="qwen3.5:9b")
    return parser.parse_args()


def validate_options(args: argparse.Namespace) -> None:
    if not SAFE_HOST.fullmatch(args.ssh_host) or args.ssh_host.startswith("-"):
        raise DemoError("SSH host contains unsupported characters")
    if not SAFE_MODEL.fullmatch(args.model) or args.model.startswith("-"):
        raise DemoError("model ID is not safe for the video label")
    if (
        ROOM_BINARY.is_symlink()
        or not ROOM_BINARY.is_file()
        or not os.access(ROOM_BINARY, os.X_OK)
    ):
        raise DemoError("build kilix-land-agent before generating the video")


def request_payload(model: str, messages: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "model": model,
        "messages": messages,
        "tools": VIDEO_TOOLS,
        "stream": False,
        "think": False,
        "keep_alive": "5m",
        "options": {
            "temperature": 0,
            "num_ctx": 8192,
            "num_predict": 128,
            "seed": 11,
        },
    }


def tool_result(name: str, content: dict[str, Any]) -> dict[str, Any]:
    return {
        "role": "tool",
        "tool_name": name,
        "content": json.dumps(content, separators=(",", ":")),
    }


def call_model(
    args: argparse.Namespace,
    messages: list[dict[str, Any]],
    expected: str,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    response = ollama_chat(
        args.ssh_host, request_payload(args.model, messages)
    )
    message, arguments = tool_call(response, expected)
    return message, arguments, metrics(response)


def validated_plan(args: argparse.Namespace) -> tuple[list[dict[str, Any]], str]:
    observation = room_observation()
    entities = observation.get("entities")
    if not isinstance(entities, list):
        raise DemoError("room observation has no entity catalog")
    target_ids = {
        entity.get("entity_id")
        for entity in entities
        if isinstance(entity, dict)
        and isinstance(entity.get("entity_id"), str)
    }
    if not {"computer", "bookshelf"}.issubset(target_ids):
        raise DemoError("required video targets are absent from the room")

    messages: list[dict[str, Any]] = [
        {
            "role": "system",
            "content": (
                "You control Kilix only through typed tools and must call "
                "exactly one tool per turn. Follow this order exactly: "
                "observe_room; go_to computer; interact computer; go_to "
                "bookshelf; interact bookshelf; face_user; say. Use only IDs "
                "from the observation. The say text must be printable ASCII, "
                "3 to 9 words, at most 63 characters, with no emoji."
            ),
        },
        {"role": "user", "content": REQUEST},
    ]
    proposals: list[dict[str, Any]] = []

    message, arguments, call_metrics = call_model(
        args, messages, "observe_room"
    )
    if arguments:
        raise DemoError("observe_room proposed unexpected arguments")
    proposals.append(
        {"tool": "observe_room", "arguments": {}, "metrics": call_metrics}
    )
    print("1/7 Qwen observe_room() -> validated", flush=True)
    messages.extend([message, tool_result("observe_room", observation)])

    for number, (name, target) in enumerate(
        (
            ("go_to", "computer"),
            ("interact", "computer"),
            ("go_to", "bookshelf"),
            ("interact", "bookshelf"),
        ),
        start=2,
    ):
        message, arguments, call_metrics = call_model(args, messages, name)
        if set(arguments) != {"target_id"} or arguments["target_id"] != target:
            raise DemoError(f"{name} did not preserve required target {target}")
        proposals.append(
            {"tool": name, "arguments": arguments, "metrics": call_metrics}
        )
        print(f"{number}/7 Qwen {name}({target}) -> validated", flush=True)
        status = "arrived" if name == "go_to" else "inspected"
        messages.extend(
            [
                message,
                tool_result(
                    name,
                    {
                        "protocol": "kilix.land.action-result/v1",
                        "status": status,
                        "target_id": target,
                        "preflight_fixture": True,
                    },
                ),
            ]
        )

    message, arguments, call_metrics = call_model(args, messages, "face_user")
    if arguments:
        raise DemoError("face_user proposed unexpected arguments")
    proposals.append(
        {"tool": "face_user", "arguments": {}, "metrics": call_metrics}
    )
    print("6/7 Qwen face_user() -> validated", flush=True)
    messages.extend(
        [
            message,
            tool_result(
                "face_user",
                {
                    "protocol": "kilix.land.action-result/v1",
                    "status": "facing_user",
                    "preflight_fixture": True,
                },
            ),
        ]
    )

    _message, arguments, call_metrics = call_model(args, messages, "say")
    if set(arguments) != {"text"} or not isinstance(arguments["text"], str):
        raise DemoError("say proposed missing or extra arguments")
    speech = arguments["text"].strip()
    if (
        not 3 <= len(speech) <= 63
        or not all(32 <= ord(character) <= 126 for character in speech)
        or not 3 <= len(speech.split()) <= 9
    ):
        raise DemoError("say text violated the short printable-ASCII policy")
    proposals.append(
        {"tool": "say", "arguments": {"text": speech}, "metrics": call_metrics}
    )
    print(f"7/7 Qwen say({speech!r}) -> validated", flush=True)
    return proposals, speech


def synthesize_speech(speech: str) -> tuple[Path, float]:
    build = PROJECT_ROOT / "build"
    build.mkdir(parents=True, exist_ok=True)
    if build.is_symlink():
        raise DemoError("refusing a symlinked build directory")
    descriptor, name = tempfile.mkstemp(
        prefix="qwen-kilix-tts-", suffix=".wav", dir=build
    )
    os.close(descriptor)
    path = Path(name)
    result = subprocess.run(
        [
            "espeak-ng",
            "-v",
            "en-us",
            "-s",
            "145",
            "-p",
            "58",
            "-a",
            "150",
            "-w",
            str(path),
            speech,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if result.returncode != 0:
        path.unlink(missing_ok=True)
        raise DemoError(
            f"TTS failed: {safe_error_text(result.stderr.strip())}"
        )
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(path),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=15,
    )
    try:
        duration = float(probe.stdout.strip())
    except ValueError as error:
        path.unlink(missing_ok=True)
        raise DemoError("could not measure synthesized speech") from error
    if probe.returncode != 0 or not 0.2 <= duration <= 4.8:
        path.unlink(missing_ok=True)
        raise DemoError(f"TTS duration {duration:.3f}s does not fit final scene")
    return path, duration


def encode_video(model: str, speech: str, audio: Path) -> None:
    output_dir = OUTPUT_PATH.parent
    output_dir.mkdir(parents=True, exist_ok=True)
    if output_dir.is_symlink() or OUTPUT_PATH.is_symlink():
        raise DemoError("refusing a symlinked video output path")
    project = PROJECT_ROOT.resolve(strict=True)
    if os.path.commonpath((project, output_dir.resolve(strict=True))) != str(project):
        raise DemoError("video output escapes the project workspace")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".qwen-kilix-demo-", suffix=".mp4", dir=output_dir
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    producer = subprocess.Popen(
        [
            str(ROOM_BINARY),
            "--video-replay",
            model,
            "computer",
            "bookshelf",
            speech,
        ],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if producer.stdout is None or producer.stderr is None:
        producer.kill()
        temporary.unlink(missing_ok=True)
        raise DemoError("could not start the trusted video renderer")
    encoder = subprocess.Popen(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "rawvideo",
            "-pixel_format",
            "rgba",
            "-video_size",
            "1280x720",
            "-framerate",
            "30",
            "-i",
            "pipe:0",
            "-i",
            str(audio),
            "-filter_complex",
            "[1:a]adelay=25000:all=1,apad[audio]",
            "-map",
            "0:v:0",
            "-map",
            "[audio]",
            "-t",
            "30",
            "-c:v",
            "libx264",
            "-preset",
            "fast",
            "-crf",
            "18",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "aac",
            "-b:a",
            "160k",
            "-ar",
            "48000",
            "-movflags",
            "+faststart",
            str(temporary),
        ],
        stdin=producer.stdout,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    producer.stdout.close()
    try:
        _encoder_output, encoder_error = encoder.communicate(timeout=300)
        producer_status = producer.wait(timeout=30)
        producer_error = producer.stderr.read().decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as error:
        encoder.kill()
        producer.kill()
        temporary.unlink(missing_ok=True)
        raise DemoError("video generation timed out") from error
    if producer_status != 0 or encoder.returncode != 0:
        temporary.unlink(missing_ok=True)
        detail = encoder_error.decode("utf-8", errors="replace")
        raise DemoError(
            "video pipeline failed: "
            f"{safe_error_text(producer_error or detail)}"
        )
    os.replace(temporary, OUTPUT_PATH)


def probe_video() -> dict[str, Any]:
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration:stream=index,codec_type,codec_name,width,height,r_frame_rate",
            "-of",
            "json",
            str(OUTPUT_PATH),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if result.returncode != 0:
        raise DemoError("ffprobe could not validate the generated video")
    try:
        probe = json.loads(result.stdout)
        duration = float(probe["format"]["duration"])
        streams = probe["streams"]
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise DemoError("generated video metadata is incomplete") from error
    video = next(
        (stream for stream in streams if stream.get("codec_type") == "video"),
        None,
    )
    audio = next(
        (stream for stream in streams if stream.get("codec_type") == "audio"),
        None,
    )
    if (
        not 29.95 <= duration <= 30.05
        or not video
        or video.get("codec_name") != "h264"
        or video.get("width") != 1280
        or video.get("height") != 720
        or video.get("r_frame_rate") != "30/1"
        or not audio
        or audio.get("codec_name") != "aac"
    ):
        raise DemoError("generated video failed duration, video, or audio gates")
    return probe


def main() -> int:
    args = parse_args()
    audio: Path | None = None
    try:
        validate_options(args)
        proposals, speech = validated_plan(args)
        audio, speech_duration = synthesize_speech(speech)
        print(f"TTS: {speech_duration:.3f}s local espeak-ng audio", flush=True)
        print("Rendering and encoding 900 frames...", flush=True)
        encode_video(args.model, speech, audio)
        probe = probe_video()
        atomic_json(
            TRANSCRIPT_PATH,
            {
                "protocol": "kilix.agent.video-transcript/v1",
                "mode": "preflight-validated-visual-replay",
                "model": args.model,
                "request": REQUEST,
                "proposals": proposals,
                "speech": speech,
                "tts": {
                    "engine": "espeak-ng",
                    "voice": "en-us",
                    "duration_seconds": round(speech_duration, 3),
                },
                "video": {
                    "path": str(OUTPUT_PATH.relative_to(PROJECT_ROOT)),
                    "duration_seconds": float(probe["format"]["duration"]),
                    "width": 1280,
                    "height": 720,
                    "frames_per_second": 30,
                    "video_codec": "h264",
                    "audio_codec": "aac",
                },
            },
        )
    except (DemoError, OSError, subprocess.SubprocessError) as error:
        print(f"qwen-video-demo: {error}", file=sys.stderr)
        return 1
    finally:
        if audio is not None:
            audio.unlink(missing_ok=True)
    print(f"Video: {OUTPUT_PATH}")
    print(f"Transcript: {TRANSCRIPT_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
