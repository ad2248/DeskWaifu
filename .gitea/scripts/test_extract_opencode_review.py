import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).with_name("extract-opencode-review.py")
SPEC = importlib.util.spec_from_file_location("extract_opencode_review", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ExtractReviewTest(unittest.TestCase):
    def write_events(self, *texts: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "opencode.jsonl"
        events = [json.dumps({"type": "text", "part": {"text": text}}) for text in texts]
        path.write_text("\n".join(events), encoding="utf-8")
        return path

    def test_extracts_complete_review(self):
        source = self.write_events(
            "## 结论\n可以合并。",
            "## 阻塞问题\n未发现阻塞问题。\n\n## 非阻塞建议\n无。",
        )

        review = MODULE.extract_review(source)

        self.assertIn("## 结论", review)
        self.assertIn("## 阻塞问题", review)
        self.assertIn("## 非阻塞建议", review)

    def test_rejects_intermediate_progress_text(self):
        source = self.write_events("阅读剩余的 diff 部分以完成审查。")

        with self.assertRaisesRegex(ValueError, "incomplete review"):
            MODULE.extract_review(source)


if __name__ == "__main__":
    unittest.main()
