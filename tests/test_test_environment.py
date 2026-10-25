import json
import os
from pathlib import Path
import subprocess
import signal
import unittest


ROOT = Path(__file__).resolve().parents[1]


class TestEnvironmentTest(unittest.TestCase):
    def test_interruption_cleans_owned_runtime(self):
        script = ('import json, os, signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); '
                  'print(json.dumps(dict(os.environ)), flush=True); time.sleep(60)')
        process = subprocess.Popen(["python3", str(ROOT / "scripts/test-environment.py"), "python3", "-c", script],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            child = json.loads(process.stdout.readline())  # child startup barrier, not a guessed delay
            self.assertTrue(Path(child["XDG_RUNTIME_DIR"]).is_dir())
            process.send_signal(signal.SIGTERM)
            _, error = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 128 + signal.SIGTERM, error)
            self.assertFalse(Path(child["XDG_RUNTIME_DIR"]).exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_allowlist_and_cleanup_on_success_and_failure(self):
        for code in (0, 12):
            with self.subTest(code=code):
                env = dict(os.environ, MINIMAX_API_KEY="do-not-inherit",
                           DESKWAIFU_AUDIO_TEST="1", DESKWAIFU_TTS_LIVE_KEY_FILE="secret",
                           WAYLAND_DISPLAY="desktop", SWAYSOCK="desktop", DISPLAY=":0",
                           HYPRLAND_INSTANCE_SIGNATURE="desktop", QT_PLUGIN_PATH="desktop")
                script = "import json, os, sys; print(json.dumps(dict(os.environ))); sys.exit(" + str(code) + ")"
                result = subprocess.run(["python3", str(ROOT / "scripts/test-environment.py"),
                                         "python3", "-c", script], env=env,
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, code, result.stderr)
                child = json.loads(result.stdout)
                for key in ("MINIMAX_API_KEY", "DESKWAIFU_AUDIO_TEST", "DESKWAIFU_TTS_LIVE_KEY_FILE",
                            "WAYLAND_DISPLAY", "SWAYSOCK", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "QT_PLUGIN_PATH"):
                    self.assertNotIn(key, child)
                self.assertIn("DBUS_SESSION_BUS_ADDRESS", child)
                self.assertNotEqual(child["HOME"], os.environ["HOME"])
                for key in ("HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"):
                    self.assertFalse(Path(child[key]).exists(), f"leaked {key}")


if __name__ == "__main__":
    unittest.main()
