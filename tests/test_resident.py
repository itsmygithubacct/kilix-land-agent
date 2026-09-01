from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from typing import Any

from agent.help_docs import HelpLibrary
from agent.protocol import RoomClient, RoomClosed, RoomRuntime
from agent.providers import DeterministicProvider, Proposal
from agent.resident import (
    LocalSpeaker,
    PersistentChatLoop,
    ResidentError,
    ResidentLoop,
)


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


class FailingSpeaker:
    def speak(self, text: str) -> None:
        del text
        raise ResidentError("test audio failure")


class ResidentLoopTests(unittest.TestCase):
    def run_with(
        self,
        provider: object,
        *,
        help_library: HelpLibrary | None = None,
        speaker: object | None = None,
        ui_updates: bool = False,
    ):
        runtime = RoomRuntime.start(headless=True)
        try:
            room = RoomClient(runtime.channel, session_id="resident-test")
            loop = ResidentLoop(
                room=room,
                provider=provider,  # type: ignore[arg-type]
                speaker=(speaker or LocalSpeaker(
                    enabled=False, model="piper-en-us-kristin-medium"
                )),  # type: ignore[arg-type]
                request="Inspect the bookshelf and report back.",
                help_library=help_library,
                ui_updates=ui_updates,
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

    def test_graphical_chat_keeps_reply_when_local_audio_fails(self) -> None:
        result = self.run_with(
            ScriptedProvider(
                [
                    ("observe_room", {}),
                    ("face_user", {}),
                    ("say", {"text": "The reply still appears in the room."}),
                ]
            ),
            speaker=FailingSpeaker(),
            ui_updates=True,
        )
        self.assertEqual(result.speech, "The reply still appears in the room.")

    def test_help_tools_return_bounded_content_without_room_capabilities(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "GUIDE.md").write_text(
                "# Radio help\n\nThe radio uses the safe audio catalog.\n",
                encoding="utf-8",
            )
            provider = ScriptedProvider(
                [
                    ("observe_room", {}),
                    ("search_help", {"query": "safe audio"}),
                    ("read_help", {"path": "GUIDE.md", "line_start": 1}),
                    ("face_user", {}),
                    ("say", {"text": "The help says the radio uses safe audio."}),
                ]
            )
            result = self.run_with(provider, help_library=HelpLibrary(root))
        self.assertEqual(
            [record.tool for record in result.actions],
            ["observe_room", "search_help", "read_help", "face_user", "say"],
        )

    def test_persistent_chat_serves_a_turn_then_waits_for_another(self) -> None:
        runtime = RoomRuntime.start(headless=True)
        client = RoomClient(runtime.channel, session_id="persistent-test")

        class QueuedRoom:
            def __init__(self):
                self.requests = ["Say hello."]

            @property
            def revision(self):
                return client.revision

            def observe(self):
                return client.observe()

            def action(self, *args, **kwargs):
                return client.action(*args, **kwargs)

            def wait_for_chat(self):
                if self.requests:
                    return self.requests.pop(0)
                raise RoomClosed("test completed")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("# Help\n", encoding="utf-8")
            loop = PersistentChatLoop(
                room=QueuedRoom(),  # type: ignore[arg-type]
                provider=ScriptedProvider(
                    [
                        ("observe_room", {}),
                        ("face_user", {}),
                        ("say", {"text": "Hello from Kilix."}),
                    ]
                ),
                speaker=LocalSpeaker(
                    enabled=False, model="piper-en-us-kristin-medium"
                ),
                help_library=HelpLibrary(root),
            )
            try:
                with self.assertRaises(RoomClosed):
                    loop.run()
                self.assertEqual(loop.history[-1]["content"], "Hello from Kilix.")
            finally:
                runtime.close()


if __name__ == "__main__":
    unittest.main()
