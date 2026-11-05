"""守护 ci_failed 回调载荷与 OpenHands 自动化表达式的契约。"""

import contextlib
import hashlib
import hmac
import importlib.util
import io
import json
import os
import re
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

def job_blocks(text):
    """按两空格缩进的作业名切分 ci.yml，无需 PyYAML 依赖。"""
    jobs: dict = {}
    current = None
    for line in text.split("\njobs:\n", 1)[1].splitlines():
        header = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
        if header:
            current = header.group(1)
            jobs[current] = []
        elif current:
            jobs[current].append(line)
    return {name: "\n".join(lines) for name, lines in jobs.items()}


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

    def test_aggregated_report_names_every_failed_job(self):
        """一次 CI 失败只能上报一次，但载荷要点名全部失败作业。

        回归：ci.yml 曾让 package 与 test 各自上报，两个作业同时失败时
        运行 722/710 各创建了两条 runs_created，重复触发智能体。
        """
        payload = self.module.build_payload(PR_EVENT, ENV | {"CI_FAILED_JOBS": "package test"})
        self.assertIn("package", payload["logs"])
        self.assertIn("test", payload["logs"])

    def test_single_failed_job_is_reported_by_name(self):
        payload = self.module.build_payload(PR_EVENT, ENV | {"CI_FAILED_JOBS": "test"})
        self.assertIn("作业「test」", payload["logs"])

    def test_falls_back_to_current_job_without_the_aggregate_variable(self):
        payload = self.module.build_payload(PR_EVENT, ENV)
        self.assertIn("作业「test」", payload["logs"])

    def test_workflow_reports_once_from_a_job_depending_on_both_ci_jobs(self):
        """锁住 ci.yml 的聚合上报结构，避免退回到每作业各报一次。"""
        jobs = job_blocks((ROOT / "workflows/ci.yml").read_text())
        reporters = [name for name, body in jobs.items() if "report-ci-failure.py" in body]
        self.assertEqual(reporters, ["report"], "上报必须集中在一个作业里，否则会重复创建智能体运行")
        report = jobs["report"]
        self.assertIn("needs: [package, test]", report)
        self.assertIn("if: always()", report)
        # 聚合步骤必须把各作业结果传出去，否则日志区只剩作业名 report。
        self.assertIn("needs.package.result", report)
        self.assertIn("needs.test.result", report)
        # 没有失败时必须自行跳过，否则一次成功也会触发回调。
        self.assertIn("CI 未失败，跳过回调", report)

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
