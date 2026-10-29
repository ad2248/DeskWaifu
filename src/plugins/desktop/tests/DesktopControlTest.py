"""Exercise real desktop command helpers without signalling the user's processes."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class DesktopControlTest(unittest.TestCase):
    def test_commands_without_a_display_or_valid_configuration(self):
        with tempfile.TemporaryDirectory(prefix="desktop cli ") as directory:
            root = Path(directory)
            log = root / "signals.jsonl"
            config = root / "invalid.json"
            config.write_text("{invalid")
            killall = root / "killall"
            killall.write_text(
                f"#!{sys.executable}\nimport json, sys\n"
                f"with open({str(log)!r}, 'a') as stream:\n"
                "    stream.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            )
            killall.chmod(0o700)
            default_root = (Path(HOST).resolve().parent.parent / "plugins").resolve() == Path(PLUGINS).resolve()
            for display in (False, True):
                environment = dict(os.environ, DESKWAIFU_PLUGIN_DIR=PLUGINS, PATH=f"{root}:{os.environ['PATH']}")
                for name in ("DISPLAY", "WAYLAND_DISPLAY", "QT_QPA_PLATFORM"):
                    environment.pop(name, None)
                if display:
                    environment.update(QT_QPA_PLATFORM="wayland", WAYLAND_DISPLAY="missing-cli-display")
                for command, signal in (("toggle", "SIGUSR1"), ("--toggle", "SIGUSR1"),
                                        ("focus", "SIGUSR2"), ("--focus", "SIGUSR2")):
                    with self.subTest(display=display, command=command):
                        result = subprocess.run([HOST, "--config", str(config), command], env=environment,
                                                capture_output=True, text=True, timeout=5)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        calls = [json.loads(line) for line in log.read_text().splitlines()]
                        self.assertEqual(calls[-1], ["-s", signal, "deskwaifu"])
                        self.assertNotIn("[Plugins]", result.stderr)
                        self.assertNotIn("[Config]", result.stderr)
                        if default_root:
                            environment.pop("DESKWAIFU_PLUGIN_DIR", None)
                            result = subprocess.run([HOST, command], env=environment,
                                                    capture_output=True, text=True, timeout=5)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            self.assertEqual(json.loads(log.read_text().splitlines()[-1]), ["-s", signal, "deskwaifu"])
                            environment["DESKWAIFU_PLUGIN_DIR"] = PLUGINS
            self.assertEqual(len(log.read_text().splitlines()), 16 if default_root else 8)


if __name__ == "__main__":
    HOST, PLUGINS = sys.argv[1:3]
    del sys.argv[1:3]
    unittest.main()
