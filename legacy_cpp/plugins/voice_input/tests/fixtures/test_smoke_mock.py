"""Stdlib contract tests for the offline smoke's local ASR mock (no desktop/audio)."""
import array
import base64
from contextlib import closing
import http.client
import importlib.util
import io
import json
import math
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, call, patch
import wave

SPEC = importlib.util.spec_from_file_location(
    "voice_input_smoke", Path(__file__).resolve().parents[1] / "voice_input_smoke.py")
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)


class MockContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="voice-mock-test-")
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        logs = root / "logs"
        logs.mkdir()
        self.smoke = SMOKE.Smoke(SimpleNamespace(logs=logs), root)
        self.smoke.start_mock()
        self.addCleanup(self.smoke.server.server_close)
        self.addCleanup(self.smoke.server.shutdown)

    def payload(self, silent=False, padding=True, dirty_padding=False):
        frames = array.array("h", [0 if silent else (12000 if i % 2 else -12000)
                                   for i in range(16000)])
        if padding:
            frames.extend([100 if dirty_padding else 0] * 4000)
        if sys.byteorder != "little":
            frames.byteswap()
        output = io.BytesIO()
        with wave.open(output, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(frames.tobytes())
        return {"model": "qwen-audio-3.0-asr-flash",
                "parameters": {"format": "wav", "sample_rate": "16000"},
                "input": {"messages": [{"role": "user", "content": [{
                    "type": "input_audio", "input_audio": {
                        "data": "data:audio/wav;base64," + base64.b64encode(output.getvalue()).decode()}}]}]}}

    def post(self, payload, path=SMOKE.API_PATH, key=SMOKE.KEY):
        with closing(http.client.HTTPConnection("127.0.0.1", self.smoke.server.server_port, timeout=5)) as client:
            client.request("POST", path, body=json.dumps(payload).encode(), headers={
                "Authorization": "Bearer " + key, "X-DashScope-SSE": "disable",
                "Content-Type": "application/json"})
            response = client.getresponse()
            return response.status, json.loads(response.read())

    def test_native_success_preserves_whitespace_and_audio(self):
        status, body = self.post(self.payload())
        self.assertEqual(status, 200)
        self.assertEqual(body["text"].encode(), SMOKE.TEXT.encode())
        self.assertTrue(body["text"].startswith("  "))
        self.assertTrue(body["text"].endswith("  \n"))
        record = dict(self.smoke.records[0])
        self.assertAlmostEqual(record.pop("capture_tail_1khz_score"), 0)
        self.assertEqual(record, {"samples": 20000, "captured_samples": 16000,
                                  "padding_samples": 4000, "peak": 12000,
                                  "capture_tail_peak": 12000, "mode": "success"})
        self.assertTrue((self.smoke.args.logs / "request-0.wav").is_file())
        self.assertEqual(self.smoke.errors, [])

    def test_intentional_asr_failure_still_validates_audio(self):
        self.smoke.mode = "failure"
        status, _ = self.post(self.payload())
        self.assertEqual(status, 503)
        self.assertEqual(len(self.smoke.records), 1)
        self.assertEqual(self.smoke.errors, [])

    def test_silence_is_a_hard_failure(self):
        status, _ = self.post(self.payload(silent=True))
        self.assertEqual(status, 400)
        self.assertEqual(self.smoke.records, [])
        self.assertIn("silent", self.smoke.errors[0])

    def test_missing_or_nonzero_silence_padding_is_rejected(self):
        for arguments in ({"padding": False}, {"dirty_padding": True}):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.post(self.payload(**arguments))[0], 400)
        self.assertEqual(len(self.smoke.errors), 2)
        self.assertTrue(all("250ms zero padding" in error for error in self.smoke.errors))
        self.assertEqual(self.smoke.records, [])

    def test_protocol_path_and_dummy_authorization_are_checked(self):
        for arguments in ({"path": "/v1/chat/completions"}, {"key": "wrong-dummy"}):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.post(self.payload(), **arguments)[0], 400)
        self.assertEqual(len(self.smoke.errors), 2)
        self.assertEqual(self.smoke.records, [])

    def test_invalid_wav_and_wrong_model_are_rejected(self):
        payload = self.payload()
        payload["model"] = "unexpected-model"
        self.assertEqual(self.post(payload)[0], 400)
        payload = self.payload()
        payload["input"]["messages"][0]["content"][0]["input_audio"]["data"] = "data:audio/wav;base64,YmFk"
        self.assertEqual(self.post(payload)[0], 400)
        self.assertEqual(len(self.smoke.errors), 2)
        self.assertEqual(self.smoke.records, [])

    def test_virtual_mic_readiness_retries_unpublished_default_metadata(self):
        status = SimpleNamespace(stdout="42. test-mic")
        inspect = SimpleNamespace(stdout='node.name = "test-mic"')
        with patch.object(self.smoke, "run", side_effect=[status, None,
                subprocess.CalledProcessError(3, "wpctl inspect"), status, None, inspect]):
            self.assertFalse(self.smoke.select_mic())
            self.assertTrue(self.smoke.select_mic())

    def test_clipboard_seed_is_foreground_registered_and_retries_no_selection(self):
        holder = Mock(pid=12343)
        holder.poll.return_value = None
        with patch.object(SMOKE.subprocess, "Popen", return_value=holder) as launch, \
                patch.object(self.smoke, "paste", side_effect=[subprocess.CalledProcessError(1, "wl-paste"),
                                                               SMOKE.SENTINEL]):
            self.smoke.seed()
        command = launch.call_args.args[0]
        self.assertEqual(command, ["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"])
        self.assertTrue(launch.call_args.kwargs["start_new_session"])
        self.assertEqual(self.smoke.processes, [holder])
        self.assertEqual((self.smoke.root / "clipboard-seed.txt").read_bytes(), SMOKE.SENTINEL)
        self.assertNotEqual(launch.call_args.kwargs["stdout"], subprocess.PIPE)

    def test_clipboard_seed_exit_is_failure_not_readiness_retry(self):
        holder = Mock()
        holder.poll.return_value = 1
        with patch.object(self.smoke, "spawn", return_value=holder), \
                patch.object(self.smoke, "paste") as paste:
            with self.assertRaisesRegex(AssertionError, "seed exited"):
                self.smoke.seed()
        paste.assert_not_called()

    def test_all_registered_groups_get_kill_even_after_leader_exit(self):
        exited = Mock(pid=12341)
        exited.wait.return_value = 0
        slow = Mock(pid=12342)
        slow.wait.side_effect = [subprocess.TimeoutExpired("fixture", 3), 0]
        self.smoke.processes = [exited, slow]
        with patch.object(SMOKE.os, "killpg") as kill:
            self.smoke.stop()
        self.assertEqual(kill.call_args_list, [call(12342, signal.SIGTERM),
                                              call(12341, signal.SIGTERM),
                                              call(12342, signal.SIGKILL),
                                              call(12341, signal.SIGKILL)])

    def test_group_errors_do_not_prevent_other_groups_being_killed(self):
        self.smoke.processes = [Mock(pid=12341), Mock(pid=12342)]
        with patch.object(SMOKE.os, "killpg", side_effect=[PermissionError("test"), None, None, None]) as kill:
            with self.assertRaises(PermissionError):
                self.smoke.stop()
        self.assertEqual(kill.call_args_list[-2:], [call(12342, signal.SIGKILL),
                                                   call(12341, signal.SIGKILL)])

    def test_cleanup_closes_server_and_writes_summary_when_stop_fails(self):
        with patch.object(self.smoke, "stop", side_effect=RuntimeError("stop failed")), \
                patch.object(self.smoke.server, "shutdown") as shutdown, \
                patch.object(self.smoke.server, "server_close") as close:
            with self.assertRaisesRegex(RuntimeError, "stop failed"):
                self.smoke.cleanup()
            shutdown.assert_called_once()
            close.assert_called_once()
        summary = json.loads((self.smoke.args.logs / "mock-requests.json").read_text())
        self.assertEqual(summary, {"requests": [], "errors": []})

    def test_cleanup_writes_summary_even_when_server_shutdown_fails(self):
        with patch.object(self.smoke.server, "shutdown", side_effect=RuntimeError("shutdown failed")), \
                patch.object(self.smoke.server, "server_close") as close:
            with self.assertRaisesRegex(RuntimeError, "shutdown failed"):
                self.smoke.cleanup()
            close.assert_called_once()
        self.assertTrue((self.smoke.args.logs / "mock-requests.json").is_file())

    def test_termination_signals_raise_for_finally_cleanup(self):
        for sig in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(signal=sig), patch.object(SMOKE.signal, "signal") as install:
                with self.assertRaises(InterruptedError):
                    SMOKE.interrupt(sig, None)
                self.assertEqual(install.call_args_list, [call(signal.SIGTERM, signal.SIG_IGN),
                                                         call(signal.SIGINT, signal.SIG_IGN)])

    def test_marker_feature_rejects_loud_non_marker_audio(self):
        tone = [int(12000 * math.sin(2 * math.pi * index / 16 + .7)) for index in range(4000)]
        self.assertGreater(SMOKE.marker_score(tone), .99)
        self.assertGreater(SMOKE.marker_score(tone[:2400] + [0] * 1600), .59)
        wrong_frequency = [int(12000 * math.sin(2 * math.pi * 700 * index / 16000)) for index in range(4000)]
        self.assertLess(SMOKE.marker_score(wrong_frequency), .01)
        self.assertEqual(SMOKE.marker_score([0] * 4000), 0)
        speech = array.array("h", (Path(__file__).parent / "voice-input.pcm").read_bytes())
        if sys.byteorder != "little":
            speech.byteswap()
        self.assertLess(SMOKE.marker_score(speech[-4000:]), .35)

    def test_environment_has_no_inherited_session_or_credentials(self):
        self.assertNotIn("DISPLAY", self.smoke.env)
        self.assertNotIn("WAYLAND_DISPLAY", self.smoke.env)
        self.assertNotIn("SWAYSOCK", self.smoke.env)
        self.assertNotIn("HTTP_PROXY", self.smoke.env)
        self.assertEqual(self.smoke.env["PULSE_SOURCE"], "test-mic")
        self.assertIn(str(self.smoke.root), self.smoke.env["PULSE_SERVER"])
        self.assertIn(str(self.smoke.root), self.smoke.env["DBUS_SESSION_BUS_ADDRESS"])
        self.assertFalse(any("API_KEY" in name for name in self.smoke.env))


if __name__ == "__main__":
    unittest.main()
