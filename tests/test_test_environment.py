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
    def test_nested_wrapper_does_not_inherit_a_long_temporary_root(self):
        with tempfile.TemporaryDirectory(prefix="nested-wrapper-") as directory:
            root = Path(directory)
            inherited = root / ("parent-" + "x" * 60)
            inherited.mkdir()
            report = root / "snapshot.json"
            probe = root / "probe.py"
            probe.write_text('''import json, os, socket, sys
from pathlib import Path
root = Path(os.environ["TMPDIR"])
fixture = root / "processor-ownership-test-123456"
fixture.mkdir()
with socket.socket(socket.AF_UNIX) as endpoint:
    endpoint.bind(str(fixture / "deskwaifu.sock"))
Path(sys.argv[1]).write_text(json.dumps({"tmp": str(root), "runtime": os.environ["XDG_RUNTIME_DIR"]}))
''')
            # 安装根不存在 /tmp/opencode；模拟外层包装器留下的长 TMPDIR。
            launcher = '''import runpy, sys, tempfile
from unittest.mock import patch
tempfile.tempdir = sys.argv.pop(1)
sys.argv = sys.argv[1:]
with patch("pathlib.Path.is_dir", return_value=False):
    runpy.run_path(sys.argv[0], run_name="__main__")
'''
            result = subprocess.run([sys.executable, "-c", launcher, str(inherited), str(ROOT / "scripts/test-environment.py"),
                                     sys.executable, str(probe), str(report)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            snapshot = json.loads(report.read_text())
            self.assertFalse(Path(snapshot["tmp"]).is_relative_to(inherited))
            self.assertFalse(Path(snapshot["tmp"]).exists())
            self.assertFalse(Path(snapshot["runtime"]).exists())

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
                'set_tests_properties(timeout-fixture PROPERTIES TIMEOUT 1 WILL_FAIL "${EXPECT_FAILURE}")\n')
            build = root / "build"
            commands = (
                ["ctest", "--test-dir", str(build), "--output-on-failure"],
                [sys.executable, str(ROOT / "scripts/run-test-gate.py"), str(build), "--test", "^timeout-fixture$"],
            )
            for expected in ("FALSE", "TRUE"):
                subprocess.run(["cmake", "-S", str(root), "-B", str(build), f"-DEXPECT_FAILURE={expected}"],
                               capture_output=True, check=True)
                for command in commands:
                    with self.subTest(expected_failure=expected, command=command[0]):
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

    def test_expected_failure_only_inverts_the_test_command(self):
        with tempfile.TemporaryDirectory(prefix="expected-failure-") as directory:
            root = Path(directory)
            (root / "empty-path").mkdir()
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(fixture NONE)\ninclude(CTest)\n'
                f'include("{ROOT}/cmake/TestEnvironment.cmake")\n'
                f'deskwaifu_add_test(NAME expected COMMAND "{sys.executable}" -c "raise SystemExit(7)")\n'
                f'deskwaifu_add_test(NAME unexpected-success COMMAND "{sys.executable}" -c "pass")\n'
                'deskwaifu_add_test(NAME missing-command COMMAND /nonexistent/deskwaifu-test)\n'
                f'deskwaifu_add_test(NAME signal COMMAND "{sys.executable}" -c "import os,signal; os.kill(os.getpid(), signal.SIGTERM)")\n'
                f'deskwaifu_add_test(NAME setup-error COMMAND "{sys.executable}" -c "raise SystemExit(7)")\n'
                f'deskwaifu_add_test(NAME expected-skip COMMAND "{sys.executable}" -c "raise SystemExit(77)")\n'
                'set_tests_properties(expected unexpected-success missing-command signal setup-error PROPERTIES WILL_FAIL TRUE TIMEOUT 2)\n'
                'set_tests_properties(expected-skip PROPERTIES WILL_FAIL TRUE SKIP_RETURN_CODE 77 TIMEOUT 2)\n'
                f'set_tests_properties(setup-error PROPERTIES ENVIRONMENT "PATH={root}/empty-path")\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
            for name in ("expected", "unexpected-success", "missing-command", "signal", "setup-error", "expected-skip"):
                commands = (
                    ["ctest", "--test-dir", str(build), "--output-on-failure", "-R", f"^{name}$"],
                    [sys.executable, str(ROOT / "scripts/run-test-gate.py"), str(build), "--test", f"^{name}$"],
                )
                for command in commands:
                    with self.subTest(test=name, command=command[0]):
                        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                        passed = name == "expected" or (name == "expected-skip" and command[0] == "ctest")
                        self.assertEqual(result.returncode == 0, passed, result.stdout + result.stderr)
                        if name == "expected-skip":
                            self.assertIn("Skipped", result.stdout)

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

    def test_job_summary_is_forwarded_without_opening_the_rest_of_the_host(self):
        script = "import json, os; print(json.dumps(dict(os.environ)))"
        with tempfile.TemporaryDirectory(prefix="job-summary-") as directory:
            summary = Path(directory) / "summary.md"
            env = dict(os.environ, GITHUB_STEP_SUMMARY=str(summary), TEST_ENV_PROBE_SECRET="secret")
            result = subprocess.run([sys.executable, str(ROOT / "scripts/test-environment.py"),
                                     sys.executable, "-c", script], env=env,
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            child = json.loads(result.stdout)
            self.assertEqual(child["GITHUB_STEP_SUMMARY"], str(summary),
                             "CI 的报告没有传进隔离环境就只能翻 job 日志")
            self.assertNotIn("TEST_ENV_PROBE_SECRET", child)

    def test_absent_job_summary_does_not_become_an_empty_entry(self):
        script = "import json, os; print(json.dumps(dict(os.environ)))"
        env = {key: value for key, value in os.environ.items() if key != "GITHUB_STEP_SUMMARY"}
        result = subprocess.run([sys.executable, str(ROOT / "scripts/test-environment.py"),
                                 sys.executable, "-c", script], env=env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("GITHUB_STEP_SUMMARY", json.loads(result.stdout))

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
