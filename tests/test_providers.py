from __future__ import annotations

import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock

from agent.providers import OllamaSshProvider, ProviderError
from agent.resident import LocalSpeaker


class OllamaProviderTests(unittest.TestCase):
    def test_validates_one_typed_proposal(self) -> None:
        response = {
            "message": {
                "role": "assistant",
                "content": "",
                "tool_calls": [
                    {
                        "function": {
                            "name": "go_to",
                            "arguments": {"target_id": "bookshelf"},
                        }
                    }
                ],
            },
            "eval_count": 10,
            "eval_duration": 500_000_000,
        }
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=json.dumps(response), stderr=""
        )
        provider = OllamaSshProvider(host="approved-test-host",
                                    model="qwen3.5:9b")
        with mock.patch("agent.providers.subprocess.run", return_value=completed) as run:
            proposal = provider.propose([{"role": "user", "content": "move"}])
        self.assertEqual(proposal.name, "go_to")
        self.assertEqual(proposal.arguments, {"target_id": "bookshelf"})
        command = run.call_args.args[0]
        self.assertEqual(command[0], "ssh")
        self.assertIn("approved-test-host", command)
        self.assertIn("http://127.0.0.1:11434/api/chat", command)
        self.assertFalse(run.call_args.kwargs.get("shell", False))

    def test_multiple_or_unknown_tools_fail_closed(self) -> None:
        for calls in (
            [],
            [
                {"function": {"name": "observe_room", "arguments": {}}},
                {"function": {"name": "face_user", "arguments": {}}},
            ],
            [{"function": {"name": "shutdown", "arguments": {}}}],
        ):
            response = {"message": {"role": "assistant", "tool_calls": calls}}
            completed = subprocess.CompletedProcess(
                args=[], returncode=0, stdout=json.dumps(response), stderr=""
            )
            provider = OllamaSshProvider(host="approved-test-host",
                                        model="qwen3.5:9b")
            with mock.patch("agent.providers.subprocess.run", return_value=completed):
                with self.assertRaises(ProviderError):
                    provider.propose([])

    def test_unsafe_transport_options_are_rejected(self) -> None:
        with self.assertRaises(ProviderError):
            OllamaSshProvider(host="-oProxyCommand=anything", model="qwen3.5:9b")
        with self.assertRaises(ProviderError):
            OllamaSshProvider(host="approved", model="../../model")


class LocalSpeakerTests(unittest.TestCase):
    def test_speech_uses_stdin_and_catalogued_model_without_a_shell(self) -> None:
        speaker = LocalSpeaker(enabled=True, model="piper-en-us-kristin-medium")
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="accepted\n", stderr=""
        )
        with mock.patch.object(
            speaker, "_trusted_binary", return_value=Path("/fixed/kilix-tts")
        ), mock.patch.object(speaker, "_ensure_daemon"), mock.patch(
            "agent.resident.subprocess.run", return_value=completed
        ) as run:
            speaker.speak("A bounded sentence.")
        self.assertEqual(
            run.call_args.args[0],
            [
                "/fixed/kilix-tts", "--speak", "-", "--model",
                "piper-en-us-kristin-medium",
            ],
        )
        self.assertEqual(run.call_args.kwargs["input"], "A bounded sentence.")
        self.assertFalse(run.call_args.kwargs.get("shell", False))

    def test_muted_speech_starts_no_process(self) -> None:
        speaker = LocalSpeaker(enabled=False, model="espeak")
        with mock.patch("agent.resident.subprocess.run") as run:
            speaker.speak("Silence.")
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
