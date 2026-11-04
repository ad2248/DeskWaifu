import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
EXTRACTOR = Path(__file__).with_name("extract-opencode-review.py")
REVIEW_SCRIPT = Path(__file__).with_name("opencode-review.sh")
REVIEW_ACTION = Path(__file__).with_name("opencode-review-action.sh")
SPEC = importlib.util.spec_from_file_location("extract_opencode_review", EXTRACTOR)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def review_event_values(script: str) -> list[str]:
    assignments = (
        line for line in script.splitlines() if not line.lstrip().startswith("#")
    )
    events = []
    for line in assignments:
        matches = re.finditer(
            r"(?<![$\w])review_event\s*=(?!=)\s*(['\"]?)([A-Za-z][A-Za-z_]*)\1"
            r"(?=\s*(?:#.*)?$)",
            line,
            re.IGNORECASE,
        )
        events.extend(match.group(2).upper() for match in matches)
    return events


class ExtractReviewTest(unittest.TestCase):
    def write_events(self, *messages: object) -> Path:
        """按消息写 JSONL；每条消息是一个字符串或若干 text part 的列表。"""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "opencode.jsonl"
        events = []
        for index, message in enumerate(messages):
            texts = [message] if isinstance(message, str) else message
            for text in texts:
                part = {"messageID": f"msg-{index:04d}", "text": text}
                events.append(json.dumps({"type": "text", "part": part}))
        path.write_text("\n".join(events), encoding="utf-8")
        return path

    def test_publishes_last_message_verbatim(self):
        review_text = "## 结论\n可以合并。\n\n## 阻塞问题\n未发现阻塞问题。\n\n## 非阻塞建议\n无。"
        source = self.write_events(
            "目标分支 checkout 的 base 与 head 内容均已确认。",
            "环境变量齐备，开始执行核验与批准流程。",
            review_text,
        )

        review = MODULE.extract_review(source)

        self.assertEqual(review_text, review)

    def test_publishes_last_message_without_format_checks(self):
        # 不校验章节标题与顺序：最后一条消息无论写成什么都原样发布。
        source = self.write_events("先看基线。", "阻塞问题：无。可以合并。")

        self.assertEqual("阻塞问题：无。可以合并。", MODULE.extract_review(source))

    def test_joins_all_text_parts_of_the_last_message(self):
        source = self.write_events(
            "过程叙述，不应发布。",
            ["第一段。", "第二段。"],
        )

        self.assertEqual("第一段。\n第二段。", MODULE.extract_review(source))

    def test_publishes_long_review_without_truncation(self):
        long_review = "## 结论\n" + "很长的评审正文。\n" * 20_000
        source = self.write_events("过程叙述。", long_review)

        review = MODULE.extract_review(source)

        self.assertEqual(long_review, review)
        self.assertNotIn("被截断", review)

    def test_keeps_events_without_message_id_separate(self):
        # 旧版输出没有 messageID：每个 text part 各自成段，取最后一段。
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "opencode.jsonl"
        path.write_text(
            "\n".join(
                json.dumps({"type": "text", "part": {"text": text}})
                for text in ("过程叙述。", "## 结论\n可以合并。")
            ),
            encoding="utf-8",
        )

        self.assertEqual("## 结论\n可以合并。", MODULE.extract_review(path))

    def test_rejects_output_without_text(self):
        source = self.write_events()

        with self.assertRaisesRegex(ValueError, "no review text"):
            MODULE.extract_review(source)

    def test_review_agent_can_approve_pull_requests(self):
        script = REVIEW_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("--agent build", script)
        self.assertNotIn('"steps":', script)
        self.assertIn('.gitea/scripts/opencode-review-action.sh approve', script)
        self.assertIn('cd "$workdir"', script)
        self.assertIn('"${server_args[@]}"', script)
        self.assertIn("last events follow", script)

    def test_final_review_uses_approval_signal(self):
        script = REVIEW_SCRIPT.read_text(encoding="utf-8")

        self.assertIn('export REVIEW_APPROVAL_FILE="$approval_file"', script)
        self.assertIn('"event": event', script)

    def test_review_events_follow_gitea_api_contract(self):
        script = REVIEW_SCRIPT.read_text(encoding="utf-8")
        events = review_event_values(script)

        self.assertTrue(events, "No review_event assignments found")
        for event in events:
            with self.subTest(event=event):
                self.assertIn(event, {"APPROVED", "COMMENT", "REQUEST_CHANGES", "PENDING"})

    def test_review_event_extraction_ignores_comments_and_quotes(self):
        self.assertEqual(
            review_event_values(
                "# review_event=INVALID\n"
                "  # review_event=INVALID\n"
                "review_event=COMMENT\n"
                "review_event=COMMENT # trailing note\n"
                "review_event=\"APPROVED\"\n"
                "review_event='REQUEST_CHANGES'\n"
                "REVIEW_EVENT=PENDING\n"
                "[[ $review_event == APPROVED ]]\n"
                "echo review_event=$review_event\n"
                "review_event==APPROVED\n"
            ),
            ["COMMENT", "COMMENT", "APPROVED", "REQUEST_CHANGES", "PENDING"],
        )

    def test_approve_action_checks_head_and_records_signal(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            log = directory / "tea.log"
            approval = directory / "approved"
            tea = directory / "tea"
            tea.write_text(
                "#!/usr/bin/env bash\n"
                "printf '%s\\n' \"$*\" >> \"$TEA_LOG\"\n"
                "if [[ \"$*\" != *'-X POST'* ]]; then\n"
                "  printf '{\"head\":{\"sha\":\"%s\"}}\\n' \"$REVIEW_HEAD_SHA\"\n"
                "fi\n",
                encoding="utf-8",
            )
            tea.chmod(0o755)
            env = os.environ | {
                "PATH": f"{directory}:{os.environ['PATH']}",
                "TEA_LOG": str(log),
                "GITHUB_REPOSITORY": "kals/DeskWaifu",
                "REVIEW_REPO_ROOT": "/trusted/checkout",
                "REVIEW_PR_NUMBER": "14",
                "REVIEW_HEAD_SHA": "abc1234",
                "REVIEW_MARKER": "<!-- opencode-review:abc1234 -->",
                "REVIEW_APPROVAL_FILE": str(approval),
            }

            result = subprocess.run(
                ["bash", str(REVIEW_ACTION), "approve", "review passed"],
                check=True,
                capture_output=True,
                env=env,
                text=True,
            )

            calls = log.read_text(encoding="utf-8")
            self.assertEqual("approved abc1234\n", result.stdout)
            self.assertIn("/repos/kals/DeskWaifu/pulls/14 --login reviewer --repo /trusted/checkout", calls)
            self.assertNotIn("-X POST", calls)
            self.assertEqual(
                "<!-- opencode-review:abc1234 -->\nreview passed\n",
                approval.read_text(encoding="utf-8"),
            )


if __name__ == "__main__":
    unittest.main()
