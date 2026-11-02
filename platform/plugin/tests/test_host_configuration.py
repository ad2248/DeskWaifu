"""The plugin host must configure without the application executable."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


HOST_SOURCE = Path(__file__).resolve().parents[1]


class HostConfigurationTest(unittest.TestCase):
    def test_standalone_host_with_and_without_gui_sdk(self):
        for gui in ("OFF", "ON"):
            with self.subTest(gui=gui), tempfile.TemporaryDirectory(prefix="plugin-host-configure-") as directory:
                result = subprocess.run([
                    "cmake", "-S", str(HOST_SOURCE), "-B", directory,
                    "-DBUILD_TESTING=ON", "-DCMAKE_CXX_STANDARD=14",
                    f"-DDESKWAIFU_BUILD_GUI_PLUGIN_SDK={gui}",
                ], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                registry = json.loads(subprocess.check_output([
                    "ctest", "--test-dir", directory, "--show-only=json-v1",
                ], text=True))
                names = {test["name"] for test in registry["tests"]}
                self.assertIn("plugin-manager", names)
                self.assertNotIn("plugin-host-link", names)


if __name__ == "__main__":
    unittest.main()
