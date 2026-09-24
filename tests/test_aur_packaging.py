"""Guard against AUR documentation paths diverging from the package name."""

from pathlib import Path
import re
import unittest


class AurPackagingTest(unittest.TestCase):
    def test_readme_is_installed_under_package_documentation_directory(self):
        pkgbuild = (Path(__file__).resolve().parents[1] / "packaging/aur/PKGBUILD").read_text()
        readme_install = re.search(r'^\s*install -Dm644 .*README\.md" "([^"]+)"', pkgbuild, re.M)
        self.assertIsNotNone(readme_install)
        self.assertEqual(readme_install.group(1), "$pkgdir/usr/share/doc/$pkgname/README.md")


if __name__ == "__main__":
    unittest.main()
