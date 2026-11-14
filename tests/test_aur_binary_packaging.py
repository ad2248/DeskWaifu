"""Offline updater tests; optional consumer checks when the submodule is present."""
import importlib.util
import io
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("update_aur", ROOT / "scripts/update-aur.py")
updater = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updater)
PRODUCER = updater.release_tool("release-packaging")
COMMIT = "a" * 40
RECIPE = """pkgver=0.1.0
pkgrel=3
_tag=0.1.0
_upstream_pkgrel=1
sha256sums=(
  'aaa'
  'bbb'
)
"""


class UpdateTest(unittest.TestCase):
    def test_new_version_resets_only_aur_release(self):
        text = updater.render_recipe(RECIPE, "0.2.0", ["ccc", "ddd"])
        self.assertIn("pkgver=0.2.0\npkgrel=1\n_tag=0.2.0\n_upstream_pkgrel=1", text)
        self.assertIn("  'ccc'\n  'ddd'", text)

    def test_same_version_preserves_aur_revision(self):
        self.assertEqual(updater.render_recipe(RECIPE, "0.1.0", ["aaa", "bbb"]), RECIPE)

    def test_ocr_dependency_constraints_are_rejected(self):
        dependencies = {"deskwaifu-ocr-bin": {"deskwaifu-bin", "opencv"}}
        info = ("pkgbase = deskwaifu-bin\n\tpkgver = 0.1.0\n\tpkgrel = 2\n"
                "\npkgname = deskwaifu-ocr-bin\n\tdepends = opencv\n\tdepends = deskwaifu-bin\n")
        updater.verify_dependencies(dependencies, info)
        for constraint in ("=0.1.0-1", ">=0.1.0", "<=0.1.0", ">0.1.0", "<0.1.0"):
            for include_plain in (False, True):
                with self.subTest(constraint=constraint, include_plain=include_plain):
                    constrained = info.replace("depends = deskwaifu-bin\n", f"depends = deskwaifu-bin{constraint}\n")
                    if include_plain:
                        constrained += "\tdepends = deskwaifu-bin\n"
                    with self.assertRaisesRegex(ValueError, "without a version constraint"):
                        updater.verify_dependencies(dependencies, constrained)

    def test_changed_existing_assets_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Assets changed"):
            updater.render_recipe(RECIPE, "0.1.0", ["ccc", "bbb"])

    def test_invalid_tags_do_not_download_or_modify(self):
        with patch.object(updater, "download") as download:
            for tag in ("latest/thing", "1.0-1", "1:1.0", "1.0\n", "$(touch nope)"):
                with self.subTest(tag=tag), self.assertRaises(ValueError):
                    updater.update(tag)
            download.assert_not_called()

    def test_dirty_submodule_is_rejected_before_download(self):
        with patch.object(updater.Path, "is_file", return_value=True), \
                patch.object(updater.subprocess, "check_output", side_effect=[str(ROOT), " M PKGBUILD"]), \
                patch.object(updater, "download") as download:
            with self.assertRaisesRegex(ValueError, "local changes"):
                updater.update("0.2.0")
            download.assert_not_called()

    def test_missing_submodule_is_actionable(self):
        with patch.object(updater.Path, "is_file", return_value=False):
            with self.assertRaisesRegex(ValueError, "Initialize"):
                updater.require_clean_submodule(ROOT / "packaging/aur")


class RustConsumerTest(unittest.TestCase):
    def test_only_one_package(self):
        self.assertEqual(updater.PACKAGES, ("deskwaifu-bin",))

    def test_template_is_binary_only(self):
        text = (ROOT / "packaging/aur-template/PKGBUILD").read_text()
        for forbidden in ("build()", "cmake", "ninja", "git+", "deskwaifu-ocr-bin"):
            self.assertNotIn(forbidden, text)
        self.assertIn("bsdtar -xf", text)
        self.assertIn("noextract=", text)
        rendered = updater.render_recipe(text, "0.9.0", ["a" * 64])
        self.assertNotIn("SKIP", rendered)
        self.assertIn("_tag=0.9.0", rendered)

    @unittest.skipUnless(shutil.which("bsdtar"), "bsdtar required")
    def test_consumer_preserves_binary_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "package"
            output.mkdir()
            archive = root / "deskwaifu-bin-0.1.0-1-x86_64.pkg.tar.zst"
            with tarfile.open(archive, "w") as stream:
                entry = tarfile.TarInfo("usr/bin/deskwaifu")
                entry.mode = 0o755
                entry.size = 6
                stream.addfile(entry, io.BytesIO(b"binary"))
            subprocess.run(["bash", "-euc", 'source "$1"; srcdir=$2; pkgdir=$3; package', "bash",
                            str(ROOT / "packaging/aur-template/PKGBUILD"), str(root), str(output)], check=True)
            self.assertEqual((output / "usr/bin/deskwaifu").stat().st_mode & 0o777, 0o755)

if __name__ == "__main__":
    unittest.main()
