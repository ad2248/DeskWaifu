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


@unittest.skipUnless(shutil.which("bsdtar"), "bsdtar is required")
class AssetTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        for name in updater.PACKAGES:
            if name == "deskwaifu-bin":
                files = {f"usr/bin/{entry}" for entry in
                         ("deskwaifu", "deskwaifu-control", "deskwaifu-ctl", "deskwaifu-cursor-guard")}
                files.update(f"usr/lib/deskwaifu/plugins/{p}/{p}.so" for p in PRODUCER.PLUGINS)
                files.update(PRODUCER.SDK_LIBRARIES)
                depends = PRODUCER.MAIN_DEPENDS
            else:
                files = {"usr/lib/deskwaifu/ocr/deskwaifu-ocr.so"}
                depends = {"deskwaifu-bin", "opencv", "yaml-cpp", "onnxruntime-cpu"}
            info = f"pkgname = {name}\npkgver = 0.1.0-1\narch = x86_64\n"
            info += "".join(f"depend = {dependency}\n" for dependency in sorted(depends))
            archive = self.directory / f"{name}-0.1.0-1-x86_64.pkg.tar.zst"
            # bsdtar reads plain tar by magic too; no compression is needed for fixtures.
            with tarfile.open(archive, "w") as stream:
                for path, data in [(".PKGINFO", info.encode()), *[(path, b"binary") for path in files]]:
                    entry = tarfile.TarInfo(path)
                    entry.size = len(data)
                    stream.addfile(entry, io.BytesIO(data))
        PRODUCER.verify(self.directory, "0.1.0", COMMIT)

    def test_valid_assets(self):
        sums, dependencies = updater.verify_assets(self.directory, "0.1.0", COMMIT)
        self.assertEqual(len(sums), 2)
        self.assertTrue(all(len(value) == 64 for value in sums))
        self.assertEqual(dependencies["deskwaifu-bin"], PRODUCER.MAIN_DEPENDS)

    def test_new_upstream_dependency_is_not_silently_dropped(self):
        archive = self.directory / "deskwaifu-bin-0.1.0-1-x86_64.pkg.tar.zst"
        with tarfile.open(archive) as stream:
            entries = [(entry, stream.extractfile(entry).read()) for entry in stream.getmembers()]
        with tarfile.open(archive, "w") as stream:
            for entry, data in entries:
                if entry.name == ".PKGINFO":
                    data += b"depend = libnotify>=0.8\n"
                    entry.size = len(data)
                stream.addfile(entry, io.BytesIO(data))
        PRODUCER.verify(self.directory, "0.1.0", COMMIT)
        _, dependencies = updater.verify_assets(self.directory, "0.1.0", COMMIT)
        info = "pkgbase = deskwaifu-bin\n\tpkgver = 0.1.0\n\tpkgrel = 2\n"
        for name, required in dependencies.items():
            info += f"\npkgname = {name}\n"
            for dep in sorted(required - {"libnotify>=0.8"}):
                info += f"\tdepends = {dep}\n"
        with self.assertRaisesRegex(ValueError, "libnotify>=0.8"):
            updater.verify_dependencies(dependencies, info)
        # Keeping the new dependency satisfies the contract.
        info = info.replace("pkgname = deskwaifu-bin\n", "pkgname = deskwaifu-bin\n\tdepends = libnotify>=0.8\n")
        updater.verify_dependencies(dependencies, info)
        with self.assertRaisesRegex(ValueError, "deskwaifu-bin"):
            updater.verify_dependencies(dependencies, info.replace("\tdepends = deskwaifu-bin\n", ""))

    def test_corrupt_or_missing_asset_is_rejected(self):
        path = next(self.directory.glob("*.pkg.tar.zst"))
        path.write_bytes(path.read_bytes() + b"corruption")
        with self.assertRaisesRegex(ValueError, "checksum"):
            updater.verify_assets(self.directory, "0.1.0", COMMIT)
        path.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            updater.verify_assets(self.directory, "0.1.0", COMMIT)

    def test_wrong_tag_or_commit_is_rejected(self):
        with self.assertRaises(ValueError):
            updater.verify_assets(self.directory, "0.2.0", COMMIT)
        with self.assertRaisesRegex(ValueError, "manifest"):
            updater.verify_assets(self.directory, "0.1.0", "b" * 40)

    def test_install_scripts_and_unexpected_payload_are_rejected(self):
        archive = next(self.directory.glob("*.pkg.tar.zst"))
        with tarfile.open(archive, "a") as stream:
            stream.addfile(tarfile.TarInfo(".INSTALL"))
        PRODUCER.verify(self.directory, "0.1.0", COMMIT)
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            updater.verify_assets(self.directory, "0.1.0", COMMIT)

    def test_verification_failure_leaves_recipe_untouched(self):
        root = self.directory / "parent"
        recipe = root / "packaging/aur"
        recipe.mkdir(parents=True)
        (recipe / "PKGBUILD").write_text(RECIPE)
        (recipe / ".SRCINFO").write_text("original metadata")
        with patch.object(updater, "ROOT", root), \
                patch.object(updater, "require_clean_submodule"), \
                patch.object(updater, "download", return_value=b"invalid"), \
                patch.object(updater, "public_tag_commit", return_value=COMMIT), \
                patch.object(updater, "verify_assets", side_effect=ValueError("checksum")):
            with self.assertRaisesRegex(ValueError, "checksum"):
                updater.update("0.2.0")
        self.assertEqual((recipe / "PKGBUILD").read_text(), RECIPE)
        self.assertEqual((recipe / ".SRCINFO").read_text(), "original metadata")


@unittest.skipUnless((ROOT / "packaging/aur/PKGBUILD").is_file(), "AUR submodule not initialized")
class ConsumerTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("bsdtar"), "bsdtar is required")
    def test_separate_payloads_preserve_permissions_links_and_license(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            recipe = ROOT / "packaging/aur/PKGBUILD"
            version = re.search(r"^pkgver=(\S+)$", recipe.read_text(), re.M).group(1)
            for package in updater.PACKAGES:
                source = root / f"upstream-{package}-{version}-1-x86_64.pkg.tar.zst"
                with tarfile.open(source, "w") as stream:
                    entries = [(".PKGINFO", b"upstream metadata"), (f"usr/bin/{package}", b"binary")]
                    if package == "deskwaifu-ocr-bin":
                        # ONNX Runtime is a system dependency, so upstream ships only model licenses.
                        entries.append(("usr/share/licenses/deskwaifu-ocr/models/LICENSE-PaddleOCR", b"Apache-2.0"))
                    for path, data in entries:
                        entry = tarfile.TarInfo(path)
                        entry.mode = 0o755 if path.startswith("usr/bin/") else 0o644
                        entry.size = len(data)
                        stream.addfile(entry, io.BytesIO(data))
                    link = tarfile.TarInfo(f"usr/bin/{package}-link")
                    link.type = tarfile.SYMTYPE
                    link.linkname = package
                    stream.addfile(link)
                output = root / package
                output.mkdir()
                subprocess.run(["bash", "-euc", 'CARCH=x86_64; source "$1"; pkgname=$2; '
                                'srcdir=$3; pkgdir=$4; "package_$pkgname"', "bash",
                                str(ROOT / "packaging/aur/PKGBUILD"), package, str(root), str(output)], check=True)
                self.assertFalse((output / ".PKGINFO").exists())
                self.assertEqual((output / f"usr/bin/{package}").stat().st_mode & 0o777, 0o755)
                self.assertEqual((output / f"usr/bin/{package}-link").readlink(), Path(package))
                other = "deskwaifu-ocr-bin" if package == "deskwaifu-bin" else "deskwaifu-bin"
                self.assertFalse((output / f"usr/bin/{other}").exists())
                if package == "deskwaifu-ocr-bin":
                    self.assertTrue((output / "usr/share/licenses/deskwaifu-ocr-bin/models/LICENSE-PaddleOCR").is_file())
                    self.assertFalse((output / "usr/share/licenses/deskwaifu-ocr-bin/onnxruntime").exists())
                    self.assertFalse((output / "usr/share/licenses/deskwaifu-ocr-bin/LICENSE").exists())

    def test_binary_recipe_has_no_build_or_sdk_download(self):
        text = (ROOT / "packaging/aur/PKGBUILD").read_text()
        for forbidden in ("build()", "cmake", "ninja", "git+", "SKIP", "pacman -U", "cubism.live2d.com"):
            self.assertNotIn(forbidden, text)
        self.assertIn("noextract=", text)
        self.assertIn("'!strip' '!debug'", text)

    def test_ocr_recipe_uses_system_onnxruntime(self):
        # Regression: 0.2.1 moved ONNX Runtime to a system dependency, so the recipe must
        # declare it and stop installing a bundled runtime license that no longer ships.
        text = (ROOT / "packaging/aur/PKGBUILD").read_text()
        _, _, ocr = text.partition("package_deskwaifu-ocr-bin()")
        self.assertTrue(ocr, "OCR split package is missing from the recipe")
        self.assertIn("'onnxruntime-cpu'", ocr)
        self.assertNotIn("onnxruntime/LICENSE", ocr)
        self.assertNotIn("libonnxruntime", ocr)

    @unittest.skipUnless(shutil.which("makepkg"), "makepkg is required")
    def test_generated_metadata_and_independent_recipe_revision(self):
        recipe = ROOT / "packaging/aur"
        actual = subprocess.check_output(["makepkg", "--printsrcinfo"], cwd=recipe, text=True)
        self.assertEqual(actual, (recipe / ".SRCINFO").read_text())
        version = re.search(r"^\s*pkgver = (\S+)$", actual, re.M).group(1)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            text = (recipe / "PKGBUILD").read_text()
            text = re.sub(r"^pkgrel=\d+$", "pkgrel=3", text, count=1, flags=re.M)
            (path / "PKGBUILD").write_text(text)
            info = subprocess.check_output(["makepkg", "--printsrcinfo"], cwd=path, text=True)
            ocr = info.split("\npkgname = deskwaifu-ocr-bin\n")[1]
            dependencies = PRODUCER.metadata("\n".join(line.strip() for line in ocr.splitlines()))["depends"]
            self.assertIn("deskwaifu-bin", dependencies)
            PRODUCER.verify_unversioned_dependencies(dependencies)
            self.assertIn(f"upstream-deskwaifu-bin-{version}-1-x86_64.pkg.tar.zst", info)


if __name__ == "__main__":
    unittest.main()
