from __future__ import annotations

import json
import socket
import unittest

from kilix_land_agent.protocol import ACTION_PROTOCOL, RESULT_PROTOCOL, RoomClient, RoomRuntime


class LiveProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.runtime = RoomRuntime.start(headless=True)

    def tearDown(self) -> None:
        status = self.runtime.close()
        self.assertIn(status, (0, -15, -9))

    @staticmethod
    def request(
        action_id: str,
        action: str,
        *,
        revision: int = 0,
        **extra: object,
    ) -> bytes:
        document: dict[str, object] = {
            "protocol": ACTION_PROTOCOL,
            "session_id": "test-session",
            "action_id": action_id,
            "action": action,
            "expected_revision": revision,
            "timeout_ms": 5000,
        }
        document.update(extra)
        return json.dumps(document, separators=(",", ":")).encode("ascii")

    def receive(self) -> dict[str, object]:
        self.runtime.channel.settimeout(3)
        document = json.loads(self.runtime.channel.recv(4096))
        self.assertIsInstance(document, dict)
        return document

    def test_complete_revisioned_action_sequence(self) -> None:
        client = RoomClient(self.runtime.channel, session_id="client-session")
        observation = client.observe()
        self.assertEqual(observation["revision"], 0)
        self.assertEqual(len(observation["entities"]), 8)
        self.assertNotIn("status", observation["available_actions"])

        status = client.action("status", text="QWEN ACTION: search_help()")
        self.assertEqual(status["status"], "ok")
        self.assertEqual(status["revision"], observation["revision"])

        arrived = client.action("go_to", target_id="bookshelf")
        self.assertEqual(arrived["status"], "ok")
        self.assertEqual(arrived["result"], "arrived")
        self.assertGreater(arrived["revision"], 0)

        interacted = client.action("interact", target_id="bookshelf")
        self.assertEqual(interacted["status"], "ok")
        faced = client.action("face_user")
        self.assertEqual(faced["result"], "facing_user")
        spoken = client.action("say", text="Hello from Kilix.")
        self.assertEqual(spoken["result"], "displayed")
        self.assertGreater(spoken["revision"], observation["revision"])

    def test_stale_revision_and_unknown_target_fail_closed(self) -> None:
        self.runtime.channel.send(
            self.request("stale", "go_to", revision=9, target_id="bookshelf")
        )
        stale = self.receive()
        self.assertEqual(stale["protocol"], RESULT_PROTOCOL)
        self.assertEqual(stale["status"], "rejected")
        self.assertEqual(stale["code"], "stale_revision")

        self.runtime.channel.send(
            self.request("unknown", "go_to", target_id="imaginary-object")
        )
        unknown = self.receive()
        self.assertEqual(unknown["status"], "rejected")
        self.assertEqual(unknown["code"], "unknown_target")

    def test_duplicate_action_id_is_rejected(self) -> None:
        packet = self.request("same-id", "observe")
        self.runtime.channel.send(packet)
        first = self.receive()
        self.assertEqual(first["protocol"], "kilix.land.observe/v1")
        self.runtime.channel.send(packet)
        duplicate = self.receive()
        self.assertEqual(duplicate["status"], "rejected")
        self.assertEqual(duplicate["code"], "duplicate_action_id")

    def test_cancel_interrupts_pending_navigation(self) -> None:
        self.runtime.channel.send(
            self.request("walk", "go_to", target_id="plant")
        )
        self.runtime.channel.send(
            self.request("cancel", "cancel", cancel_action_id="walk")
        )
        responses = {document["action_id"]: document
                     for document in (self.receive(), self.receive())}
        self.assertEqual(responses["walk"]["status"], "canceled")
        self.assertEqual(responses["walk"]["code"], "user_cancelled")
        self.assertEqual(responses["cancel"]["status"], "ok")

    def test_extra_field_and_oversized_packet_are_rejected(self) -> None:
        invalid = json.loads(self.request("extra", "observe"))
        invalid["command"] = "anything"
        self.runtime.channel.send(
            json.dumps(invalid, separators=(",", ":")).encode("ascii")
        )
        extra = self.receive()
        self.assertEqual(extra["session_id"], "")
        self.assertEqual(extra["code"], "unknown_field")

        self.runtime.channel.send(b"x" * 5000)
        oversized = self.receive()
        self.assertEqual(oversized["code"], "message_too_large")

    def test_busy_session_and_power_action_are_rejected(self) -> None:
        self.runtime.channel.send(
            self.request("walk", "go_to", target_id="plant")
        )
        self.runtime.channel.send(
            self.request("second", "interact", target_id="plant")
        )
        responses = {document["action_id"]: document
                     for document in (self.receive(), self.receive())}
        self.assertEqual(responses["second"]["status"], "rejected")
        self.assertEqual(responses["second"]["code"], "busy")
        self.assertEqual(responses["walk"]["status"], "ok")

        for serial, action in enumerate(("shutdown", "edit_file", "run_command")):
            self.runtime.channel.send(
                self.request(f"forbidden-{serial}", action)
            )
            forbidden = self.receive()
            self.assertEqual(forbidden["session_id"], "")
            self.assertEqual(forbidden["code"], "unknown_action")


if __name__ == "__main__":
    unittest.main()
