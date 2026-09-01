from __future__ import annotations

import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock

from agent.providers import OllamaSshProvider, ProviderError, TOOLS
from agent.resident import LocalSpeaker


class OllamaProviderTests(unittest.TestCase):
    def test_help_tools_are_read_only_and_bounded(self) -> None:
        tools = {item["function"]["name"]: item["function"] for item in TOOLS}
        self.assertIn("search_help", tools)
        self.assertIn("read_help", tools)
        self.assertEqual(
            tools["read_help"]["parameters"]["required"],
            ["path", "line_start"],
        )
        self.assertNotIn("write_help", tools)
        self.assertNotIn("status", tools)

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
        self.assertEqual(command[0], "/usr/bin/ssh")
        self.assertEqual(command[5], "--")
        self.assertIn("approved-test-host", command)
        self.assertIn("/usr/bin/curl", command)
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

    def test_retries_one_qwen_xml_tool_serialization_failure(self) -> None:
        failure = subprocess.CompletedProcess(
            args=[], returncode=22,
            stdout=json.dumps(
                {"error": "XML syntax error: element closed incorrectly"}
            ),
            stderr="curl: HTTP 500",
        )
        success = subprocess.CompletedProcess(
            args=[], returncode=0,
            stdout=json.dumps(
                {
                    "message": {
                        "role": "assistant",
                        "tool_calls": [
                            {"function": {"name": "face_user", "arguments": {}}}
                        ],
                    }
                }
            ),
            stderr="",
        )
        provider = OllamaSshProvider(host="approved-test-host",
                                    model="qwen3.5:9b")
        with mock.patch(
            "agent.providers.subprocess.run", side_effect=(failure, success)
        ) as run:
            proposal = provider.propose([{"role": "user", "content": "hello"}])
        self.assertEqual(proposal.name, "face_user")
        self.assertEqual(run.call_count, 2)
        retry_payload = json.loads(run.call_args.kwargs["input"])
        self.assertIn("previous tool serialization", retry_payload["messages"][-1]["content"])

    def test_final_reply_omits_tools_and_returns_plain_content(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[], returncode=0,
            stdout=json.dumps(
                {"message": {"role": "assistant", "content": "Press Escape."}}
            ),
            stderr="",
        )
        provider = OllamaSshProvider(host="approved-test-host",
                                    model="qwen3.5:9b")
        with mock.patch(
            "agent.providers.subprocess.run", return_value=completed
        ) as run:
            reply = provider.final_reply([{"role": "user", "content": "help"}])
        self.assertEqual(reply.text, "Press Escape.")
        payload = json.loads(run.call_args.kwargs["input"])
        self.assertNotIn("tools", payload)
        self.assertIn("printable ASCII", payload["messages"][-1]["content"])

    def test_unsafe_transport_options_are_rejected(self) -> None:
        with self.assertRaises(ProviderError):
            OllamaSshProvider(host="-oProxyCommand=anything", model="qwen3.5:9b")
        with self.assertRaises(ProviderError):
            OllamaSshProvider(host="approved", model="../../model")

    def test_room_close_terminates_an_inflight_ssh_request(self) -> None:
        class HangingProcess:
            def __init__(self):
                self.returncode = None
                self.terminated = False

            def poll(self):
                return self.returncode

            def communicate(self, input=None, timeout=None):
                del input
                if self.terminated:
                    self.returncode = -15
                    return "", ""
                raise subprocess.TimeoutExpired([], timeout)

            def terminate(self):
                self.terminated = True

            def kill(self):
                self.terminated = True
                self.returncode = -9

        checks = iter((False, True))
        process = HangingProcess()
        provider = OllamaSshProvider(
            host="approved-test-host", model="qwen3.5:9b"
        )
        provider.set_cancel_check(lambda: next(checks))
        with mock.patch(
            "agent.providers.subprocess.Popen", return_value=process
        ):
            with self.assertRaisesRegex(ProviderError, "room closed"):
                provider.propose([])
        self.assertTrue(process.terminated)


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
