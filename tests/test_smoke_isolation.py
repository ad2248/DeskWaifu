"""Exercise the real smoke launcher against disposable desktop sentinels."""

import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SmokeIsolationTest(unittest.TestCase):
    def test_failed_compositor_preserves_inherited_desktop(self):
        with tempfile.TemporaryDirectory(prefix="smoke-safety-") as directory:
            root = Path(directory)
            runtime = root / "desktop"
            runtime.mkdir(mode=0o700)
            config = root / "config"
            config.mkdir()
            (config / "sentinel").write_text("user configuration")
            programs = root / "bin"
            programs.mkdir()
            # Deliberately fail after environment setup, before any real compositor starts.
            sway = programs / "sway"
            sway.write_text('#!/bin/bash\nprintf "%s\\n" "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$SWAYSOCK" > "$DESKWAIFU_DEPS_DIR/observed"\nexit 9\n')
            sway.chmod(0o700)
            deps = root / "deps"
            (deps / "CubismSdkForNative-5-r.5/Samples/Resources/Haru").mkdir(parents=True)
            (deps / "glew").mkdir()
            with socket.socket(socket.AF_UNIX) as wayland, socket.socket(socket.AF_UNIX) as app:
                wayland.bind(str(runtime / "wayland-9"))
                app.bind(str(runtime / "deskwaifu.sock"))
                (runtime / "wayland-9.lock").write_text("desktop lock")
                env = dict(os.environ, PATH=f"{programs}:{os.environ['PATH']}",
                           XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config),
                           WAYLAND_DISPLAY="wayland-9", SWAYSOCK="desktop-control",
                           DESKWAIFU_DEPS_DIR=str(deps))
                env.pop("DESKWAIFU_TEST_ISOLATED", None)  # Exercise the public launcher, even under the outer gate.
                result = subprocess.run(["bash", str(ROOT / "scripts/headless-smoke-test.sh"),
                                         str(root / "build")], cwd=ROOT, env=env,
                                        capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertTrue((runtime / "wayland-9").is_socket(), result.stderr)
                self.assertTrue((runtime / "deskwaifu.sock").is_socket(), result.stderr)
                self.assertEqual((runtime / "wayland-9.lock").read_text(), "desktop lock")
                self.assertEqual((config / "sentinel").read_text(), "user configuration")
                observed = (deps / "observed").read_text().splitlines()
                self.assertNotEqual(observed[0], str(runtime))
                self.assertNotEqual(observed[1], str(config))
                self.assertEqual(observed[2], "")
                self.assertFalse(Path(observed[0]).exists(), "owned runtime leaked on failure")


if __name__ == "__main__":
    unittest.main()
