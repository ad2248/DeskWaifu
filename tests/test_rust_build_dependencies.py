import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RustBuildDependencyDocumentationTest(unittest.TestCase):
    def test_installer_checks_alsa_and_system_onnx_runtime(self):
        installer = (ROOT / "install.sh").read_text()
        self.assertIn('check_pkg_config alsa', installer)
        self.assertIn("libonnxruntime\\.so", installer)
        self.assertIn("alsa-lib onnxruntime-cpu", installer)

    def test_arch_source_package_declares_runtime_libraries(self):
        package = (ROOT / "packaging/arch/deskwaifu-git/PKGBUILD").read_text()
        self.assertIn("'onnxruntime-cpu'", package)
        self.assertIn("'alsa-lib'", package)

    def test_readme_separates_rust_links_from_optional_cpp_ocr(self):
        readme = (ROOT / "README.md").read_text()
        self.assertIn("The Rust ONNX link is unconditional", readme)
        self.assertIn("alsa-lib", readme)
        self.assertIn("onnxruntime-cpu", readme)
        self.assertIn("opencv", readme)
        self.assertIn("only needed when building that C++ OCR module", readme)
        self.assertNotIn("Ubuntu / Debian", readme)
        self.assertNotIn("**Fedora:**", readme)


if __name__ == "__main__":
    unittest.main()
