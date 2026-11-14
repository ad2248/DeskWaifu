"""Plugin command dispatch must precede GUI, configuration and module loading."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class PluginCommandTest(unittest.TestCase):
    def test_headless_command_and_ambiguous_provider(self):
        with tempfile.TemporaryDirectory(prefix="plugin cli ") as directory:
            root = Path(directory)
            config = root / "invalid.json"
            config.write_text("{invalid")
            command = root / "one/commands/probe"
            command.parent.mkdir(parents=True)
            command.write_text('#!/bin/sh\nprintf "handled\\n"\nexit 17\n')
            command.chmod(0o700)
            environment = dict(os.environ, DESKWAIFU_PLUGIN_DIR=str(root), QT_QPA_PLATFORM="wayland",
                               WAYLAND_DISPLAY="missing-cli-display")
            environment.pop("DISPLAY", None)
            result = subprocess.run([HOST, "--config", str(config), "probe"], env=environment,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 17, result.stderr)
            self.assertEqual(result.stdout, "handled\n")
            other = root / "two/commands/probe"
            other.parent.mkdir(parents=True)
            other.symlink_to(command)
            result = subprocess.run([HOST, "probe"], env=environment, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("Ambiguous plugin command", result.stderr)
            self.assertNotIn("handled", result.stdout)


if __name__ == "__main__":
    HOST = sys.argv.pop(1)
    unittest.main()
