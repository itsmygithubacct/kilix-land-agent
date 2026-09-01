from __future__ import annotations

import json
import socket
import unittest

from agent.protocol import CHAT_PROTOCOL, ProtocolError, RoomClient


class ChatProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.client_socket, self.room_socket = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET
        )
        self.client = RoomClient(self.client_socket, session_id="chat-test-session")

    def tearDown(self) -> None:
        self.client_socket.close()
        self.room_socket.close()

    def send(self, **changes: object) -> None:
        document: dict[str, object] = {
            "protocol": CHAT_PROTOCOL,
            "session_id": "chat-test-session",
            "message_id": "chat-00000001",
            "text": "Please walk to the radio.",
        }
        document.update(changes)
        self.room_socket.send(
            json.dumps(document, separators=(",", ":")).encode("ascii")
        )

    def test_accepts_one_exact_bounded_chat_message(self) -> None:
        self.send()
        self.assertEqual(self.client.wait_for_chat(), "Please walk to the radio.")

    def test_detects_room_channel_close_without_consuming_messages(self) -> None:
        self.assertFalse(self.client.is_closed())
        self.send()
        self.assertFalse(self.client.is_closed())
        self.assertEqual(self.client.wait_for_chat(), "Please walk to the radio.")
        self.room_socket.close()
        self.assertTrue(self.client.is_closed())

    def test_extra_fields_and_wrong_session_fail_closed(self) -> None:
        self.send(command="run anything")
        with self.assertRaisesRegex(ProtocolError, "malformed"):
            self.client.wait_for_chat()

        self.send(session_id="different-session")
        with self.assertRaisesRegex(ProtocolError, "wrong session"):
            self.client.wait_for_chat()

    def test_blank_or_non_ascii_text_is_rejected(self) -> None:
        self.send(text="   ")
        with self.assertRaisesRegex(ProtocolError, "blank"):
            self.client.wait_for_chat()

        document = {
            "protocol": CHAT_PROTOCOL,
            "session_id": "chat-test-session",
            "message_id": "chat-00000002",
            "text": "hello \N{SNOWMAN}",
        }
        self.room_socket.send(json.dumps(document).encode("utf-8"))
        with self.assertRaisesRegex(ProtocolError, "printable ASCII"):
            self.client.wait_for_chat()


if __name__ == "__main__":
    unittest.main()
