#!/usr/bin/env python3
"""Run one ordinary test with private files, D-Bus and no desktop/credentials.

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
    if sys.argv[1] == "--expect-failure":
        try:
            code = subprocess.run(sys.argv[3:]).returncode
        except OSError as error:
            print(f"Unable to start expected-failure test: {error}", file=sys.stderr)
            return 1
        if sys.argv[2] and code == int(sys.argv[2]):
            return code
        return 0 if code > 0 else 1
    timeout = float(os.environ.get("DESKWAIFU_TEST_TIMEOUT", "0"))
    command = sys.argv[1:]
    if os.environ.get("DESKWAIFU_TEST_WILL_FAIL") == "1":
        prefix = []
        if len(command) >= 3 and command[1:3] == ["-E", "env"]:
            prefix, command = command[:3], command[3:]
            while command and "=" in command[0]:
                prefix.append(command.pop(0))
        command = [*prefix, sys.executable, str(Path(__file__).resolve()), "--expect-failure",
                   os.environ.get("DESKWAIFU_TEST_SKIP_CODE", ""), *command]
    # 嵌套包装器也从短固定父目录建立私有根，不能叠加继承的 TMPDIR。
    parent = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else "/tmp"
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
        process = subprocess.Popen(["dbus-run-session", "--", *command],
                                   env=env, start_new_session=True)

        def interrupted(number, _frame):
            raise InterruptedError(number)

        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, interrupted)
        try:
            result = process.wait(timeout=timeout if timeout > 0 else None)
        except subprocess.TimeoutExpired:
            print(f"Test timed out after {timeout:g} seconds", file=sys.stderr, flush=True)
            result = 124
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
