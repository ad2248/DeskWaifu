#!/usr/bin/env python3
"""Run ordinary tests with private files, D-Bus and no inherited desktop/credentials.

This is environment isolation, not a network/filesystem security sandbox. Tests
must still replace cloud/clipboard/audio boundaries; live tests use separate,
explicitly authorized entry points, not this launcher.
"""

import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: test-environment.py COMMAND [ARG ...]")
    parent = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="deskwaifu-test-", dir=parent) as directory:
        root = Path(directory)
        for name in ("home", "runtime", "config", "cache", "data", "tmp"):
            (root / name).mkdir(mode=0o700)
        env = {
            "PATH": os.environ.get("PATH", os.defpath), "LANG": "C.UTF-8",
            "HOME": str(root / "home"), "XDG_RUNTIME_DIR": str(root / "runtime"),
            "XDG_CONFIG_HOME": str(root / "config"), "XDG_CACHE_HOME": str(root / "cache"),
            "XDG_DATA_HOME": str(root / "data"), "TMPDIR": str(root / "tmp"),
            "QT_QPA_PLATFORM": "offscreen", "QT_QPA_PLATFORMTHEME": "generic",
            "QT_NO_XDG_DESKTOP_PORTAL": "1", "LIBGL_ALWAYS_SOFTWARE": "1",
            "GALLIUM_DRIVER": "llvmpipe", "EGL_PLATFORM": "surfaceless",
            "PYTHONDONTWRITEBYTECODE": "1", "DESKWAIFU_TEST_ISOLATED": "1",
        }
        mesa = Path("/usr/share/glvnd/egl_vendor.d/50_mesa.json")
        if mesa.is_file():
            env["__EGL_VENDOR_LIBRARY_FILENAMES"] = str(mesa)
        if "DESKWAIFU_DEPS_DIR" in os.environ:
            env["DESKWAIFU_DEPS_DIR"] = os.environ["DESKWAIFU_DEPS_DIR"]
        process = subprocess.Popen(["dbus-run-session", "--", *sys.argv[1:]],
                                   env=env, start_new_session=True)

        def interrupted(number, _frame):
            raise InterruptedError(number)

        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, interrupted)
        try:
            result = process.wait()
        except InterruptedError as error:
            result = 128 + error.args[0]
        finally:
            # Include grandchildren (compositors/helpers), including on command failure.
            for number in (signal.SIGINT, signal.SIGTERM):
                signal.signal(number, signal.SIG_IGN)
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            # The leader may already have exited while a grandchild ignores TERM.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        return result if result >= 0 else 128 - result


if __name__ == "__main__":
    sys.exit(main())
