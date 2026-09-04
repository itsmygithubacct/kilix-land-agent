"""Private socketpair transport for the authoritative Studio runtime."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import secrets
import select
import socket
import stat
import subprocess
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
ROOM_BINARY = PROJECT_ROOT / "kilix-land-agent"
ACTION_PROTOCOL = "kilix.land.action/v1"
OBSERVE_PROTOCOL = "kilix.land.observe/v1"
RESULT_PROTOCOL = "kilix.land.action-result/v1"
CHAT_PROTOCOL = "kilix.land.chat-input/v1"
MAX_MESSAGE_BYTES = 4096
MAX_ACTIONS = 4096
MAX_CHAT_TEXT = 256
SUPPORTED_ACTIONS = frozenset(
    {"observe", "go_to", "interact", "face_user", "say", "status", "cancel"}
)


class ProtocolError(RuntimeError):
    """The trusted room or local transport violated its fixed contract."""


class RoomClosed(ProtocolError):
    """The user closed the graphical room and its private transport."""


def _plain_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _safe_token(value: object, *, field: str) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= 32:
        raise ProtocolError(f"{field} must be a 1 to 32 character token")
    if not all(character.isascii() and (character.isalnum() or character in "._:-")
               for character in value):
        raise ProtocolError(f"{field} contains unsupported characters")
    return value


def _validate_binary(path: Path) -> None:
    try:
        metadata = path.stat(follow_symlinks=False)
    except OSError as error:
        raise ProtocolError("build kilix-land-agent before starting a resident") from error
    if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise ProtocolError("the room runtime must be a regular non-symlink file")
    if metadata.st_uid not in (0, os.getuid()) or metadata.st_mode & 0o022:
        raise ProtocolError("the room runtime has unsafe ownership or write permissions")
    if not os.access(path, os.X_OK):
        raise ProtocolError("the room runtime is not executable")


@dataclass
class RoomRuntime:
    """One room child and its unshared, inherited SOCK_SEQPACKET channel."""

    process: subprocess.Popen[bytes]
    channel: socket.socket

    @classmethod
    def start(cls, *, headless: bool, chat: bool = False) -> "RoomRuntime":
        if headless and chat:
            raise ProtocolError("chat mode requires a graphical room")
        _validate_binary(ROOM_BINARY)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        command = [str(ROOM_BINARY), "--agent-session-fd", str(child.fileno())]
        if headless:
            command.append("--headless")
        elif chat:
            command.append("--chat")
        try:
            process = subprocess.Popen(command, cwd=PROJECT_ROOT,
                                       pass_fds=(child.fileno(),))
        except BaseException:
            parent.close()
            child.close()
            raise
        child.close()
        return cls(process=process, channel=parent)

    def close(self, *, timeout: float = 5.0) -> int:
        self.channel.close()
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                return self.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                return self.process.wait(timeout=timeout)


class RoomClient:
    """Revision-tracking client that sends one typed action at a time."""

    def __init__(self, channel: socket.socket, *, session_id: str | None = None):
        self.channel = channel
        self.session_id = _safe_token(
            session_id or f"session-{secrets.token_hex(8)}", field="session_id"
        )
        self.revision = 0
        self._serial = 0
        self.current_action_id: str | None = None

    def _next_action_id(self) -> str:
        if self._serial >= MAX_ACTIONS:
            raise ProtocolError("the session action budget is exhausted")
        self._serial += 1
        return f"action-{self._serial:04d}"

    def _receive(self, action_id: str, timeout_ms: int) -> dict[str, Any]:
        self.channel.settimeout(timeout_ms / 1000 + 2.0)
        try:
            payload, _ancillary, flags, _address = self.channel.recvmsg(
                MAX_MESSAGE_BYTES, 0
            )
        except (OSError, TimeoutError) as error:
            raise ProtocolError("the room did not return an action result") from error
        if flags & socket.MSG_TRUNC or len(payload) >= MAX_MESSAGE_BYTES:
            raise ProtocolError("the room returned an oversized message")
        if not payload:
            raise RoomClosed("the room closed the private session")
        try:
            document = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ProtocolError("the room returned invalid JSON") from error
        if not isinstance(document, dict):
            raise ProtocolError("the room result is not an object")
        protocol = document.get("protocol")
        if protocol not in (OBSERVE_PROTOCOL, RESULT_PROTOCOL):
            raise ProtocolError("the room returned an unsupported protocol")
        if document.get("session_id") != self.session_id:
            code = document.get("code")
            suffix = f" ({code})" if isinstance(code, str) else ""
            raise ProtocolError(f"the room returned the wrong session{suffix}")
        if document.get("action_id") != action_id:
            raise ProtocolError("the room returned the wrong action ID")
        revision = document.get("revision")
        if not _plain_int(revision) or revision < self.revision:
            raise ProtocolError("the room returned a regressing or invalid revision")
        self.revision = revision
        return document

    def action(
        self,
        name: str,
        *,
        target_id: str | None = None,
        text: str | None = None,
        cancel_action_id: str | None = None,
        timeout_ms: int = 10_000,
    ) -> dict[str, Any]:
        if name not in SUPPORTED_ACTIONS:
            raise ProtocolError(f"unsupported room action: {name}")
        if not _plain_int(timeout_ms) or not 1 <= timeout_ms <= 30_000:
            raise ProtocolError("timeout_ms must be between 1 and 30000")
        action_id = self._next_action_id()
        request: dict[str, object] = {
            "protocol": ACTION_PROTOCOL,
            "session_id": self.session_id,
            "action_id": action_id,
            "action": name,
            "expected_revision": self.revision,
            "timeout_ms": timeout_ms,
        }
        if target_id is not None:
            request["target_id"] = _safe_token(target_id, field="target_id")
        if text is not None:
            if not isinstance(text, str) or not 1 <= len(text) <= 512:
                raise ProtocolError("speech must contain 1 to 512 characters")
            if not all(character.isascii() and 32 <= ord(character) <= 126
                       for character in text):
                raise ProtocolError("speech must be printable ASCII")
            request["text"] = text
        if cancel_action_id is not None:
            request["cancel_action_id"] = _safe_token(
                cancel_action_id, field="cancel_action_id"
            )
        encoded = json.dumps(request, separators=(",", ":")).encode("ascii")
        if len(encoded) >= MAX_MESSAGE_BYTES:
            raise ProtocolError("the action request is oversized")
        self.current_action_id = action_id
        try:
            sent = self.channel.send(encoded)
            if sent != len(encoded):
                raise ProtocolError("the private transport truncated an action")
            return self._receive(action_id, timeout_ms)
        except OSError as error:
            raise ProtocolError("the private room transport failed") from error
        finally:
            self.current_action_id = None

    def observe(self) -> dict[str, Any]:
        document = self.action("observe", timeout_ms=2_000)
        if document.get("protocol") != OBSERVE_PROTOCOL:
            raise ProtocolError("observe did not return a room observation")
        entities = document.get("entities")
        if not isinstance(entities, list) or not entities:
            raise ProtocolError("the room observation has no entities")
        identifiers: set[str] = set()
        for entity in entities:
            if not isinstance(entity, dict):
                raise ProtocolError("the entity catalog contains a non-object")
            identifier = _safe_token(entity.get("entity_id"), field="entity_id")
            if identifier in identifiers:
                raise ProtocolError("the entity catalog contains a duplicate ID")
            identifiers.add(identifier)
        return document

    def is_closed(self) -> bool:
        """Return promptly when the authoritative room closed its channel."""

        try:
            readable, _writable, _exceptional = select.select(
                [self.channel], [], [], 0
            )
            if not readable:
                return False
            payload = self.channel.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT)
            return payload == b""
        except BlockingIOError:
            return False
        except (OSError, ValueError):
            return True

    def wait_for_chat(self) -> str:
        """Wait for one bounded user message submitted by the room composer."""

        self.channel.settimeout(None)
        try:
            payload, _ancillary, flags, _address = self.channel.recvmsg(
                MAX_MESSAGE_BYTES, 0
            )
        except OSError as error:
            raise ProtocolError("the room chat transport failed") from error
        if flags & socket.MSG_TRUNC or len(payload) >= MAX_MESSAGE_BYTES:
            raise ProtocolError("the room returned an oversized chat message")
        if not payload:
            raise RoomClosed("the room closed the private session")
        try:
            document = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ProtocolError("the room returned invalid chat JSON") from error
        if not isinstance(document, dict) or set(document) != {
            "protocol", "session_id", "message_id", "text"
        }:
            raise ProtocolError("the room returned a malformed chat message")
        if document.get("protocol") != CHAT_PROTOCOL:
            raise ProtocolError("the room returned an unsupported chat protocol")
        if document.get("session_id") != self.session_id:
            raise ProtocolError("the room chat message has the wrong session")
        _safe_token(document.get("message_id"), field="message_id")
        text = document.get("text")
        if not isinstance(text, str) or not 1 <= len(text) <= MAX_CHAT_TEXT:
            raise ProtocolError("chat text must contain 1 to 256 characters")
        if not all(character.isascii() and 32 <= ord(character) <= 126
                   for character in text):
            raise ProtocolError("chat text must be printable ASCII")
        if not text.strip():
            raise ProtocolError("chat text cannot be blank")
        return text
