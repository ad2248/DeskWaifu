"""Guard parallel PR triggers and the separation between review and CI."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/opencode-review.sh"


class ParallelReviewTest(unittest.TestCase):
    def test_review_starts_on_pr_events_without_waiting_for_ci(self):
        review = (ROOT / "workflows/opencode-review.yml").read_text()
        ci = (ROOT / "workflows/ci.yml").read_text()
        self.assertIn("  pull_request_target:\n", review)
        for workflow in (review, ci):
            self.assertIn("types: [opened, reopened, synchronize]", workflow)
            self.assertNotIn("needs:", workflow)
        self.assertNotIn("workflow_run", review)
        self.assertIn("ref: ${{ gitea.event.pull_request.base.sha }}", review)
        self.assertNotIn("ref: main", review)
        self.assertNotIn("ref: ${{ gitea.event.pull_request.head.sha }}", review)
        self.assertIn("run: bash .gitea/scripts/opencode-review.sh", review)
        self.assertIn("REVIEW_PR_NUMBER: ${{ gitea.event.pull_request.number }}", review)
        self.assertIn("REVIEW_HEAD_SHA: ${{ gitea.event.pull_request.head.sha }}", review)
        self.assertNotIn("REVIEW_CI_URL", review)

    def test_prompt_distinguishes_local_verification_from_ci(self):
        source = SCRIPT.read_text()
        prompt = source.split('\nprompt="$(cat <<EOF\n', 1)[1].split('\nEOF\n)"', 1)[0]
        self.assertIn("独立 CI 可能正在并行运行", prompt)
        self.assertIn("按需执行构建或测试", prompt)
        self.assertIn("不要将本地验证等同于 CI 通过", prompt)
        self.assertIn("测试覆盖缺口", prompt)
        self.assertNotIn("CI 已成功", source)
        self.assertNotIn("Successful CI", source)
        self.assertNotIn("REVIEW_CI_URL", source)

    def test_published_review_does_not_claim_ci_passed(self):
        source = SCRIPT.read_text()
        publish = source.split("\nreview_event=COMMENT\n", 1)[1]
        publish = "review_event=COMMENT\n" + publish.split('\ntea api ', 1)[0]
        for approved in (False, True):
            with self.subTest(approved=approved), tempfile.TemporaryDirectory() as temp:
                work = Path(temp)
                (work / "review.md").write_text("## 结论\n审查结果")
                approval = work / "approved"
                if approved:
                    approval.write_text("approved")
                subprocess.run(
                    ["bash", "-eu", "-c", publish], check=True, capture_output=True,
                    env=os.environ | {
                        "workdir": temp, "approval_file": str(approval),
                        "marker": "<!-- opencode-review:abc1234 -->",
                        "tested_sha": "abc1234",
                    },
                )
                payload = json.loads((work / "payload.json").read_text())
                self.assertEqual(payload["event"], "APPROVED" if approved else "COMMENT")
                self.assertEqual(payload["commit_id"], "abc1234")
                self.assertIn("构建与自动化测试由独立 CI 流程负责", payload["body"])
                self.assertNotIn("已核验 CI", payload["body"])

    def test_changed_head_stops_publication(self):
        source = SCRIPT.read_text()
        check = source.split('\ncurrent_sha="', 1)[1].split('\nreview_event=', 1)[0]
        check = 'current_sha="' + check
        for head in ("abc1234", "def5678"):
            with self.subTest(head=head):
                result = subprocess.run(
                    ["bash", "-eu", "-c", 'tea() { printf \'%s\' "$PR_JSON"; }\n' + check],
                    capture_output=True, text=True,
                    env=os.environ | {
                        "PR_JSON": json.dumps({"head": {"sha": head}}),
                        "repo": "kals/DeskWaifu", "pr_number": "14", "tested_sha": "abc1234",
                        "repo_root": "/trusted/checkout",
                    },
                )
                self.assertEqual(result.returncode, 0 if head == "abc1234" else 4)
                if head != "abc1234":
                    self.assertIn("discard stale result", result.stderr)


if __name__ == "__main__":
    unittest.main()
