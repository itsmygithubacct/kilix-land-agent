from __future__ import annotations

import contextlib
import io
import unittest
from typing import Any

from agent.protocol import RoomClient, RoomRuntime
from agent.providers import DeterministicProvider, Proposal
from agent.resident import LocalSpeaker, ResidentError, ResidentLoop


class ScriptedProvider:
    label = "test/scripted"

    def __init__(self, steps: list[tuple[str, dict[str, Any]]]):
        self.steps = steps
        self.index = 0

    def propose(self, messages: list[dict[str, Any]]) -> Proposal:
        del messages
        name, arguments = self.steps[self.index]
        self.index += 1
        return Proposal(
            name=name,
            arguments=arguments,
            message={
                "role": "assistant",
                "content": "",
                "tool_calls": [
                    {"function": {"name": name, "arguments": arguments}}
                ],
            },
            metrics={},
        )


class ResidentLoopTests(unittest.TestCase):
    def run_with(self, provider: object):
        runtime = RoomRuntime.start(headless=True)
        try:
            room = RoomClient(runtime.channel, session_id="resident-test")
            loop = ResidentLoop(
                room=room,
                provider=provider,  # type: ignore[arg-type]
                speaker=LocalSpeaker(enabled=False,
                                     model="piper-en-us-kristin-medium"),
                request="Inspect the bookshelf and report back.",
            )
            with contextlib.redirect_stdout(io.StringIO()):
                return loop.run()
        finally:
            runtime.close()

    def test_deterministic_provider_closes_the_live_loop(self) -> None:
        result = self.run_with(
            DeterministicProvider(
                target_id="bookshelf",
                speech="The bookshelf is ready for another story.",
            )
        )
        self.assertEqual(
            [record.tool for record in result.actions],
            ["observe_room", "go_to", "interact", "face_user", "say"],
        )
        self.assertTrue(all(record.status in ("observed", "ok")
                            for record in result.actions))

    def test_unobserved_target_is_never_sent_to_the_room(self) -> None:
        provider = ScriptedProvider(
            [
                ("observe_room", {}),
                ("go_to", {"target_id": "invented"}),
            ]
        )
        with self.assertRaisesRegex(ResidentError, "unobserved target"):
            self.run_with(provider)

    def test_extra_arguments_fail_closed(self) -> None:
        provider = ScriptedProvider(
            [("observe_room", {"unexpected": True})]
        )
        with self.assertRaisesRegex(ResidentError, "additional arguments"):
            self.run_with(provider)

    def test_say_requires_face_user(self) -> None:
        provider = ScriptedProvider(
            [
                ("observe_room", {}),
                ("say", {"text": "I skipped an action."}),
            ]
        )
        with self.assertRaisesRegex(ResidentError, "requires a successful"):
            self.run_with(provider)


if __name__ == "__main__":
    unittest.main()
