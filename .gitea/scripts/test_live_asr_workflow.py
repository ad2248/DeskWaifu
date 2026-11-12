"""Keep opt-in cloud credentials out of the ordinary test gate."""
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest


WORKFLOW = Path(__file__).resolve().parents[1] / "workflows/ci.yml"


class LiveAsrWorkflowTest(unittest.TestCase):
    def block(self):
        return WORKFLOW.read_text().split("      - name: Test live Bailian ASR when configured\n", 1)[1].split("      - name:", 1)[0]

    def run_step(self, key="", endpoint=""):
        script = textwrap.dedent(self.block().split("        run: |\n", 1)[1]).strip()
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "build-ci/bin/voice-input-live-test"
            program.parent.mkdir(parents=True)
            program.write_text("#!/bin/sh\nprintf 'LIVE_EXECUTED\\n'\n")
            program.chmod(0o700)
            return subprocess.run(["bash", "-eu", "-c", script], cwd=directory,
                                  env=dict(os.environ, BAILIAN_API_KEY=key, BAILIAN_ASR_ENDPOINT=endpoint),
                                  text=True, capture_output=True, timeout=5)

    def test_both_missing_explicitly_disabled(self):
        result = self.run_step()
        self.assertEqual(result.returncode, 0)
        self.assertIn("disabled", result.stdout)
        self.assertNotIn("LIVE_EXECUTED", result.stdout)

    def test_partial_configuration_fails(self):
        for key, endpoint in [("secret-must-not-be-printed", ""), ("", "wss://asr.invalid/api-ws/v1/inference")]:
            with self.subTest(key_present=bool(key)):
                result = self.run_step(key, endpoint)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("LIVE_EXECUTED", result.stdout)
                self.assertNotIn("secret-must-not-be-printed", result.stdout + result.stderr)

    def test_both_configured_run_separate_live_entry(self):
        result = self.run_step("secret-must-not-be-printed", "wss://asr.invalid/api-ws/v1/inference")
        self.assertEqual(result.returncode, 0)
        self.assertIn("LIVE_EXECUTED", result.stdout)
        self.assertNotIn("secret-must-not-be-printed", result.stdout + result.stderr)

    def test_live_entry_pins_requested_websocket_model(self):
        source = (WORKFLOW.parents[2] / "plugins/voice_input/tests/LiveAsrTest.cpp").read_text()
        self.assertIn('qEnvironmentVariable("BAILIAN_ASR_ENDPOINT")', source)
        self.assertIn('config.model = QStringLiteral("qwen-audio-3.1-asr-flash-streaming");', source)
        self.assertIn('QStringLiteral("wss://")', source)
        self.assertIn("NativeAsrClient client;", source)
        self.assertIn("client.Recognize(1, pcm, config)", source)
        self.assertNotIn("BAILIAN_OPENAI_ENDPOINT", source)
        self.assertNotIn("BAILIAN_OPENAI_ENDPOINT", self.block())

    def test_secret_scope_and_external_pr_guard(self):
        block = self.block()
        self.assertIn("github.event.pull_request.head.repo.full_name == github.repository", block)
        self.assertIn("BAILIAN_API_KEY: ${{ secrets.BAILIAN_API_KEY }}", block)
        self.assertIn("BAILIAN_ASR_ENDPOINT: ${{ vars.BAILIAN_ASR_ENDPOINT }}", block)
        self.assertNotIn("set -x", block)
        # test-environment deliberately scrubs credentials: live must use its own entry.
        self.assertNotIn("test-environment.py", block)


if __name__ == "__main__":
    unittest.main()
