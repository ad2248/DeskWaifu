import json
import os
from pathlib import Path
import subprocess
import signal
import shutil
import tempfile
import unittest
import sys


ROOT = Path(__file__).resolve().parents[1]


class TestEnvironmentTest(unittest.TestCase):
    def test_ctest_timeout_cleans_directories_and_processes(self):
        script = '''import json, os, signal, subprocess, sys, time
from pathlib import Path
grandchild = subprocess.Popen([sys.executable, "-c",
    "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print('ready',flush=True); time.sleep(60)"],
    stdout=subprocess.PIPE, text=True)
assert grandchild.stdout.readline().strip() == "ready"
signal.signal(signal.SIGTERM, signal.SIG_IGN)
keys = ("HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR")
snapshot = {key: os.environ[key] for key in keys}
snapshot.update(pid=os.getpid(), grandchild=grandchild.pid, group=os.getpgrp())
Path(sys.argv[1]).write_text(json.dumps(snapshot))
time.sleep(60)
'''
        with tempfile.TemporaryDirectory(prefix="ctest-timeout-") as directory:
            root = Path(directory)
            report = root / "snapshot.json"
            child = root / "child.py"
            child.write_text(script)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(fixture NONE)\ninclude(CTest)\n'
                f'include("{ROOT}/cmake/TestEnvironment.cmake")\n'
                f'deskwaifu_add_test(NAME timeout-fixture COMMAND "{sys.executable}" "{child}" "{report}")\n'
                'set_tests_properties(timeout-fixture PROPERTIES TIMEOUT 1)\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
            commands = (
                ["ctest", "--test-dir", str(build), "--output-on-failure"],
                [sys.executable, str(ROOT / "scripts/run-test-gate.py"), str(build), "--test", "^timeout-fixture$"],
            )
            for command in commands:
                with self.subTest(command=command[0]):
                    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
                    self.assertNotEqual(result.returncode, 0, "a timeout must fail the test")
                    self.assertIn("Test timed out after 1 seconds", result.stdout + result.stderr)
                    snapshot = json.loads(report.read_text())
                    try:
                        for key in ("HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"):
                            self.assertFalse(Path(snapshot[key]).exists(), f"CTest timeout leaked {key}")
                        for key in ("pid", "grandchild"):
                            status = Path(f"/proc/{snapshot[key]}/status")
                            try:
                                contents = status.read_text()
                            except FileNotFoundError:
                                continue
                            self.assertIn("State:\tZ", contents, f"CTest timeout left {key} running")
                    finally:
                        # 失败的旧实现也只回收本用例记录的进程组与隔离目录。
                        try:
                            os.killpg(snapshot["group"], signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        shutil.rmtree(Path(snapshot["HOME"]).parent, ignore_errors=True)

    def test_parallel_ctest_has_private_directories_and_bus(self):
        probe = '''import json, os, socket, sys, time
from pathlib import Path
output, name = Path(sys.argv[1]), sys.argv[2]
assert "TEST_ENV_PROBE_SECRET" not in os.environ
assert os.environ["DESKWAIFU_SCREENSHOT_TEST_BUS"] == "1"
keys = ("HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR", "DBUS_SESSION_BUS_ADDRESS")
with socket.socket(socket.AF_UNIX) as endpoint:
    endpoint.bind(str(Path(os.environ["XDG_RUNTIME_DIR"]) / "same.sock"))
    state = Path(os.environ["HOME"]) / "same-state"
    state.write_text(name)
    (output / name).write_text(json.dumps({key: os.environ[key] for key in keys}))
    deadline = time.monotonic() + 8
    while len(list(output.iterdir())) != 12:
        assert time.monotonic() < deadline, "CTest did not run twelve tests concurrently"
        time.sleep(.01)
    assert state.read_text() == name, "another test overwrote HOME"
'''
        with tempfile.TemporaryDirectory(prefix="parallel-environment-") as directory:
            root = Path(directory)
            output = root / "results with spaces"
            output.mkdir()
            script = root / "probe.py"
            script.write_text(probe)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(fixture NONE)\ninclude(CTest)\n'
                f'include("{ROOT}/cmake/TestEnvironment.cmake")\n'
                'add_executable(probe IMPORTED)\n'
                f'set_target_properties(probe PROPERTIES IMPORTED_LOCATION "{sys.executable}")\n'
                'foreach(name RANGE 1 12)\n'
                f'  deskwaifu_add_test(NAME isolation-${{name}} COMMAND probe "{script}" "{output}" ${{name}} ENVIRONMENT "DESKWAIFU_SCREENSHOT_TEST_BUS=1")\n'
                '  set_tests_properties(isolation-${name} PROPERTIES TIMEOUT 12)\n'
                'endforeach()\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
            commands = (
                [sys.executable, str(ROOT / "scripts/run-test-gate.py"), str(build), "--test", "^isolation-"],
                ["ctest", "--test-dir", str(build), "--parallel", "12", "--output-on-failure"],
            )
            for command in commands:
                with self.subTest(command=command[0]):
                    result = subprocess.run(command, env=dict(os.environ, TEST_ENV_PROBE_SECRET="secret"),
                                            capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    snapshots = [json.loads(path.read_text()) for path in output.iterdir()]
                    self.assertEqual(len(snapshots), 12)
                    for key in snapshots[0]:
                        self.assertEqual(len({env[key] for env in snapshots}), 12, f"shared {key}")
                        if key != "DBUS_SESSION_BUS_ADDRESS":
                            self.assertTrue(all(not Path(env[key]).exists() for env in snapshots), f"leaked {key}")
                    for path in output.iterdir():
                        path.unlink()

    def test_boundary_fixtures_honor_tmpdir(self):
        script = """import os, runpy, sys, tempfile
from pathlib import Path
original = tempfile.TemporaryDirectory
def private_directory(*args, **kwargs):
    directory = original(*args, **kwargs)
    if not Path(directory.name).is_relative_to(Path(os.environ["TMPDIR"])):
        directory.cleanup()
        raise AssertionError("boundary fixture ignored TMPDIR")
    return directory
tempfile.TemporaryDirectory = private_directory
path = sys.argv.pop(1)
runpy.run_path(path, run_name="__main__")
"""
        cases = (
            ("platform/tooling/BoundaryTest.py", "Boundaries.test_own_headers_and_sdk_links_are_allowed"),
            ("tests/test_build_boundaries.py", "BuildBoundaries.test_c_include_ownership"),
        )
        with tempfile.TemporaryDirectory(prefix="boundary-tmpdir-") as directory:
            for path, case in cases:
                with self.subTest(path=path):
                    result = subprocess.run(
                        ["python3", "-B", "-c", script, str(ROOT / path), case],
                        env=dict(os.environ, TMPDIR=directory), capture_output=True, text=True, timeout=20,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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
                            HYPRLAND_INSTANCE_SIGNATURE="desktop", QT_PLUGIN_PATH="desktop",
                            DESKWAIFU_PLUGIN_DIR="host-plugins", DESKWAIFU_CONTROL_PLUGIN_DIR="host-plugins")
                script = "import json, os, sys; print(json.dumps(dict(os.environ))); sys.exit(" + str(code) + ")"
                result = subprocess.run(["python3", str(ROOT / "scripts/test-environment.py"),
                                         "python3", "-c", script], env=env,
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, code, result.stderr)
                child = json.loads(result.stdout)
                for key in ("MINIMAX_API_KEY", "DESKWAIFU_AUDIO_TEST", "DESKWAIFU_TTS_LIVE_KEY_FILE",
                             "WAYLAND_DISPLAY", "SWAYSOCK", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "QT_PLUGIN_PATH",
                             "DESKWAIFU_PLUGIN_DIR", "DESKWAIFU_CONTROL_PLUGIN_DIR"):
                    self.assertNotIn(key, child)
                self.assertIn("DBUS_SESSION_BUS_ADDRESS", child)
                self.assertNotEqual(child["HOME"], os.environ["HOME"])
                for key in ("HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"):
                    self.assertFalse(Path(child[key]).exists(), f"leaked {key}")


if __name__ == "__main__":
    unittest.main()
