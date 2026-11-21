"""Stdlib contract tests for the offline smoke's local ASR mock (no desktop/audio)."""
import array
import base64
from contextlib import closing
import http.client
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
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



if __name__ == "__main__":
    unittest.main()
