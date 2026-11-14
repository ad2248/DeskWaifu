"""Keep documentation links valid without reintroducing retired identifiers."""

from pathlib import Path
import re
import unittest
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]


class DocumentationLinksTest(unittest.TestCase):
    def test_links_do_not_reintroduce_retired_identifier(self):
        legacy_name = "waifu" + "land"
        for document in (ROOT / "docs").rglob("*.md"):
            for link in re.findall(r"\[[^\]]*\]\(([^)\s]+)\)", document.read_text()):
                with self.subTest(document=document.relative_to(ROOT), link=link):
                    self.assertNotIn(legacy_name, link)

    def test_lifecycle_upgrade_link_targets_readme(self):
        document = ROOT / "docs/需求描述/产品范围与进程生命周期.md"
        link = re.search(r"\[根 README 升级指南\]\(([^)]+)\)", document.read_text())
        self.assertIsNotNone(link)
        target = urlsplit(link.group(1))
        self.assertEqual((document.parent / unquote(target.path)).resolve(), ROOT / "README.md")
        self.assertTrue((ROOT / "README.md").is_file())


if __name__ == "__main__":
    unittest.main()
