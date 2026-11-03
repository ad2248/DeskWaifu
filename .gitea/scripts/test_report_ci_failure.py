"""守护 ci_failed 回调载荷与 OpenHands 自动化表达式的契约。"""

import contextlib
import hashlib
import hmac
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "scripts/report-ci-failure.py"

ENV = {
    "GITHUB_REPOSITORY": "kals/DeskWaifu",
    "GITHUB_SERVER_URL": "https://git.kals.top",
    "GITHUB_RUN_ID": "681",
    "GITHUB_WORKFLOW": "ci.yml",
    "GITHUB_JOB": "test",
}

PR_EVENT = {"pull_request": {"number": 274, "head": {"ref": "nia/ci-openhands-callback"}}}


def load_module():
    spec = importlib.util.spec_from_file_location("report_ci_failure", MODULE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReportCiFailureTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()

    def test_push_to_default_branch_is_skipped(self):
        self.assertIsNone(self.module.build_payload({"ref": "refs/heads/main"}, ENV))
        self.assertIsNone(self.module.build_payload({"pull_request": {"number": 0}}, ENV))

    def test_pr_failure_payload_supplies_every_field_the_automation_reads(self):
        payload = self.module.build_payload(PR_EVENT, ENV)
        self.assertEqual(payload["action"], "ci_failed")
        self.assertEqual(payload["pull_request_number"], 274)
        self.assertEqual(payload["pull_request"], {"number": 274})
        self.assertEqual(payload["branch"], "nia/ci-openhands-callback")
        self.assertEqual(payload["run_url"], "https://git.kals.top/kals/DeskWaifu/actions/runs/681")
        self.assertIn("test", payload["logs"])
        self.assertTrue(payload["logs"])

    def test_turn_text_expression_renders_without_error(self):
        try:
            from jmespath import search
        except ImportError:
            self.skipTest("未安装 jmespath")
        payload = self.module.build_payload(PR_EVENT, ENV)
        text = search(
            "join('', ['CI 失败: ', repository.full_name, ' 分支 ', branch, ' PR#', "
            "to_string(pull_request_number), ' 运行: ', run_url, ' 失败日志: ', logs])",
            payload,
        )
        self.assertIn("kals/DeskWaifu", text)
        self.assertIn("PR#274", text)
        self.assertIn("actions/runs/681", text)

    def test_signature_is_hex_hmac_of_the_exact_body(self):
        body = json.dumps({"action": "ci_failed"}).encode()
        expected = hmac.new(b"secret", body, hashlib.sha256).hexdigest()
        self.assertEqual(self.module.sign(body, b"secret"), expected)

    def test_missing_secret_fails_instead_of_posting_unsigned(self):
        with tempfile.TemporaryDirectory() as temp:
            event_path = Path(temp) / "event.json"
            event_path.write_text(json.dumps(PR_EVENT))
            saved = {key: os.environ.get(key) for key in ENV}
            os.environ.update(ENV | {"GITHUB_EVENT_PATH": str(event_path)})
            os.environ.pop("PIPELINE_WEBHOOK_SECRET", None)
            self.module.EVENTS_URL = "http://127.0.0.1:1/unreachable"
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(self.module.main(), 1)
            finally:
                for key, value in saved.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value


if __name__ == "__main__":
    unittest.main()
