import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


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
                'cmake_minimum_required(VERSION 3.16)\nproject(fixture NONE)\nenable_testing()\n'
                f'add_test(NAME fixture COMMAND [=[{sys.executable}]=] [=[{script}]=])\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
            result = subprocess.run([sys.executable, str(gate.ROOT / "scripts/run-test-gate.py"),
                                     str(build), "--test", "^fixture$"], capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unexpected QTest skip: fixture/T::late", result.stderr)

    def test_truncated_report_is_not_trusted(self):
        registry = {"tests": [{"name": "critical"}]}
        report = ET.fromstring('<testsuite><testcase name="critical"><system-out>[This part of the test output was removed since it exceeds the threshold]</system-out></testcase></testsuite>')
        policy = {"required": ["critical"], "optional_cases": {}, "optional_processes": {}}
        self.assertIn("truncated output cannot prove internal case coverage: critical", gate.evaluate(registry, report, policy)["errors"])
    def test_reruns_preserve_previous_evidence(self):
        with tempfile.TemporaryDirectory(prefix="gate-report-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.16)\nproject(fixture NONE)\nenable_testing()\nadd_test(NAME fixture COMMAND "${CMAKE_COMMAND}" -E true)\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
            for _ in range(2):
                result = subprocess.run([sys.executable, str(gate.ROOT / "scripts/run-test-gate.py"),
                                         str(build), "--test", "^fixture$"], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
            reports = list((build / "test-logs").glob("ctest-*/summary.json"))
            self.assertEqual(len(reports), 2, "a diagnostic rerun must not overwrite the first report")

    def test_missing_registration_is_not_green(self):
        result = gate.evaluate({"tests": []}, ET.fromstring("<testsuite/>"),
                               {"required": ["critical"], "optional_cases": {}, "optional_processes": {}})
        self.assertIn("required test not registered: critical", result["errors"])

    def test_internal_skip_is_not_a_pass(self):
        registry = {"tests": [{"name": "critical"}]}
        report = ET.fromstring('<testsuite><testcase name="critical"><system-out>SKIP   : T::critical() Missing device</system-out></testcase></testsuite>')
        policy = {"required": ["critical"], "optional_cases": {}, "optional_processes": {}}
        self.assertIn("unexpected QTest skip: critical/T::critical", gate.evaluate(registry, report, policy)["errors"])
        policy["optional_cases"]["T::critical"] = "authorized only"
        result = gate.evaluate(registry, report, policy)
        self.assertFalse(result["errors"])
        self.assertEqual(result["tests"][0]["status"], "partial")
        self.assertEqual(result["tests"][0]["process_status"], "passed")
        self.assertEqual(result["tests"][0]["unverified_cases"][0]["reason"], "authorized only")

    def test_ctest_skip_and_missing_result_fail_required_gate(self):
        registry = {"tests": [{"name": "critical"}, {"name": "lost"}]}
        report = ET.fromstring('<testsuite><testcase name="critical" status="notrun"><skipped/></testcase></testsuite>')
        policy = {"required": ["critical"], "optional_cases": {}, "optional_processes": {}}
        result = gate.evaluate(registry, report, policy)
        self.assertIn("required test skipped: critical", result["errors"])
        self.assertIn("registered test produced no result: lost", result["errors"])

    def test_failure_and_scenario_filter(self):
        registry = {"tests": [{"name": "one"}, {"name": "two"}]}
        report = ET.fromstring('<testsuite><testcase name="one"><failure>broken</failure></testcase></testsuite>')
        policy = {"required": ["one", "two", "missing"], "optional_cases": {}, "optional_processes": {}}
        self.assertEqual(gate.evaluate(registry, report, policy, "^one$")["errors"], ["test failed: one"])


if __name__ == "__main__":
    unittest.main()
