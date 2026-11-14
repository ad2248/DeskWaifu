"""SDK 独立入口默认启用测试时也必须能完整注册隔离测试。"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SdkConfigurationTest(unittest.TestCase):
    def test_standalone_default_tests_with_and_without_gui_sdk(self):
        for gui in ("OFF", "ON"):
            with self.subTest(gui=gui), tempfile.TemporaryDirectory(prefix="sdk-config-") as directory:
                build = Path(directory) / "build"
                result = subprocess.run(
                    ["cmake", "-S", str(ROOT / "sdk"), "-B", str(build),
                     f"-DDESKWAIFU_BUILD_GUI_PLUGIN_SDK={gui}"],
                    capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("BUILD_TESTING:BOOL=ON", (build / "CMakeCache.txt").read_text())
                registry = json.loads(subprocess.check_output(
                    ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
                self.assertCountEqual([test["name"] for test in registry["tests"]], [
                    "plugin-events", "plugin-timeout-flood", "plugin-legacy-events", "plugin-sdk-image-file",
                    "plugin-sdk-contracts", "plugin-sdk-ownership", "plugin-sdk-installed-consumer"])
                for test in registry["tests"]:
                    self.assertTrue(any(arg.endswith("/scripts/test-environment.py") for arg in test["command"]))


if __name__ == "__main__":
    unittest.main()
