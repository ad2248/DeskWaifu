"""Guard AUR package paths and public source metadata."""

from pathlib import Path
import re
import unittest


class AurPackagingTest(unittest.TestCase):
    def test_cli_python_runtime_dependency(self):
        root = Path(__file__).resolve().parents[1]
        recipe = root / "packaging/arch/deskwaifu-git"
        self.assertTrue((root / "deskwaifu-ctl").read_text().startswith("#!/usr/bin/env python3\n"))
        self.assertIn("'python'", (recipe / "PKGBUILD").read_text())
        self.assertIn("\tdepends = python\n", (recipe / ".SRCINFO").read_text())

    def test_aur_build_disables_automated_tests(self):
        recipe = Path(__file__).resolve().parents[1] / "packaging/arch/deskwaifu-git"
        pkgbuild = (recipe / "PKGBUILD").read_text()
        self.assertIn("-DBUILD_TESTING=OFF", pkgbuild)
        self.assertNotIn("-DBUILD_TESTING=ON", pkgbuild)
        self.assertNotRegex(pkgbuild, r"(?m)^check\s*\(\s*\)")
        self.assertNotIn("ctest", pkgbuild)
        self.assertNotRegex(pkgbuild, r"(?m)^checkdepends=")
        self.assertNotIn("\tcheckdepends =", (recipe / ".SRCINFO").read_text())

    def test_recipe_and_metadata_use_github_main(self):
        root = Path(__file__).resolve().parents[1]
        recipe = root / "packaging/arch/deskwaifu-git"
        pkgbuild = (recipe / "PKGBUILD").read_text()
        srcinfo = (recipe / ".SRCINFO").read_text()
        url = "https://github.com/ad2248/DeskWaifu"
        source = f"deskwaifu::git+{url}.git#branch=main"
        self.assertIn(f"url='{url}'", pkgbuild)
        self.assertIn(f"'{source}'", pkgbuild)
        self.assertIn(f"\turl = {url}\n", srcinfo)
        self.assertIn(f"\tsource = {source}\n", srcinfo)
        pkgrel = re.search(r"^pkgrel=(\d+)$", pkgbuild, re.M)
        self.assertIsNotNone(pkgrel)
        self.assertIn(f"\tpkgrel = {pkgrel.group(1)}\n", srcinfo)
        for path in (recipe / "README.md", root / "README.md"):
            self.assertIn(f"git clone {url}.git", path.read_text())

    def test_rust_runtime_build_dependencies_and_sdk_root(self):
        pkgbuild = (Path(__file__).resolve().parents[1] / "packaging/arch/deskwaifu-git/PKGBUILD").read_text()
        self.assertIn("'rust'", pkgbuild)
        self.assertIn("'cargo'", pkgbuild)
        self.assertIn('-DCUBISM_SDK_ROOT="$srcdir/$_sdk"', pkgbuild)

    def test_rust_runtime_build_tracks_catalog_sources(self):
        cmake = (Path(__file__).resolve().parents[1] / "CMakeLists.txt").read_text()
        self.assertIn('${CMAKE_CURRENT_SOURCE_DIR}/rust/model-domain/src/*.rs', cmake)
        self.assertIn('${CMAKE_CURRENT_SOURCE_DIR}/rust/model-domain/Cargo.toml', cmake)

    def test_rust_runtime_native_dependencies_are_declared(self):
        # libonnxruntime.so.1 and libasound.so.2 come from the Rust runtime
        # binary; PKGBUILD and .SRCINFO must agree or an installed package
        # resolves neither at runtime.
        recipe = Path(__file__).resolve().parents[1] / "packaging/arch/deskwaifu-git"
        depends = re.search(r"^depends=\((.*)\)$", (recipe / "PKGBUILD").read_text(), re.M)
        self.assertIsNotNone(depends)
        declared = re.findall(r"'([^']+)'", depends.group(1))
        srcinfo = (recipe / ".SRCINFO").read_text()
        for dependency in ("onnxruntime-cpu", "alsa-lib"):
            self.assertIn(dependency, declared)
            self.assertIn(f"\tdepends = {dependency}\n", srcinfo)

    def test_readme_is_installed_under_package_documentation_directory(self):
        pkgbuild = (Path(__file__).resolve().parents[1] / "packaging/arch/deskwaifu-git/PKGBUILD").read_text()
        readme_install = re.search(r'^\s*install -Dm644 .*README\.md" "([^"]+)"', pkgbuild, re.M)
        self.assertIsNotNone(readme_install)
        self.assertEqual(readme_install.group(1), "$pkgdir/usr/share/doc/$pkgname/README.md")


if __name__ == "__main__":
    unittest.main()
