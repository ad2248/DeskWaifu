import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


spec = importlib.util.spec_from_file_location("gate", Path(__file__).resolve().parents[1] / "scripts/run-test-gate.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class TestGateTest(unittest.TestCase):
    def test_skip_after_ctests_default_output_limit_is_not_hidden(self):
        with tempfile.TemporaryDirectory(prefix="gate-long-output-") as directory:
            root = Path(directory)
            script = root / "long.py"
            script.write_text('print("x" * 4096)\nprint("SKIP   : T::late() critical protection absent")\n')
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(fixture NONE)\nenable_testing()\n'
                f'include("{gate.ROOT}/cmake/TestEnvironment.cmake")\n'
                f'deskwaifu_add_test(NAME fixture COMMAND [=[{sys.executable}]=] [=[{script}]=])\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
            result = subprocess.run([sys.executable, str(gate.ROOT / "scripts/run-test-gate.py"),
                                     str(build), "--test", "^fixture$"], capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unexpected QTest skip: fixture/T::late", result.stderr)

    def test_reruns_preserve_previous_evidence(self):
        with tempfile.TemporaryDirectory(prefix="gate-report-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.25)\nproject(fixture NONE)\nenable_testing()\nadd_test(NAME fixture COMMAND "${CMAKE_COMMAND}" -E true)\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
            for _ in range(2):
                result = subprocess.run([sys.executable, str(gate.ROOT / "scripts/run-test-gate.py"),
                                         str(build), "--test", "^fixture$"], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
            reports = list((build / "test-logs").glob("ctest-*/summary.json"))
            self.assertEqual(len(reports), 2, "a diagnostic rerun must not overwrite the first report")

if __name__ == "__main__":
    unittest.main()
