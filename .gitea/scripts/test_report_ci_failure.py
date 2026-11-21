"""守护 ci_failed 回调载荷与 OpenHands 自动化表达式的契约。"""

import importlib.util
import re
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
