"""构建边界用例必须完整、独立地注册，并保留隔离与失败保护。"""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

import test_build_boundaries as boundaries


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gate", ROOT / "scripts/run-test-gate.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class BuildBoundaryRegistrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="boundary-registration-")
        cls.addClassCleanup(cls.directory.cleanup)
        root = Path(cls.directory.name)
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(fixture NONE)\ninclude(CTest)\n'
            f'include("{ROOT}/cmake/TestEnvironment.cmake")\n'
            f'include("{ROOT}/tests/BuildBoundaryTests.cmake")\n')
        build = root / "build"
        subprocess.run(["cmake", "-S", str(root), "-B", str(build)], capture_output=True, check=True)
        cls.registry = json.loads(subprocess.check_output(
            ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
        cls.methods = unittest.defaultTestLoader.getTestCaseNames(boundaries.BuildBoundaries)
        cls.expected = {"build-boundaries-" + name.removeprefix("test_").replace("_", "-"): name
                        for name in cls.methods}
        cls.policy = json.loads((ROOT / "tests/gate.json").read_text())

    def test_every_method_is_registered_once_and_required(self):
        names = [test["name"] for test in self.registry["tests"]]
        self.assertTrue(self.expected)
        self.assertCountEqual(names, self.expected)
        required = [name for name in self.policy["required"] if name.startswith("build-boundaries-")]
        self.assertCountEqual(required, self.expected)
        self.assertNotIn("build-boundaries", self.policy["required"])

    def test_each_test_selects_one_method_without_serialization(self):
        for test in self.registry["tests"]:
            with self.subTest(test=test["name"]):
                command = test["command"]
                self.assertTrue(any(arg.endswith("/scripts/test-environment.py") for arg in command))
                self.assertTrue(any(arg.endswith("/tests/test_build_boundaries.py") for arg in command))
                self.assertEqual([arg for arg in command if arg.startswith("BuildBoundaries.")],
                                 ["BuildBoundaries." + self.expected[test["name"]]])
                self.assertIn("-v", command)
                properties = {item["name"]: item["value"] for item in test["properties"]}
                self.assertNotIn("TIMEOUT", properties)
                self.assertFalse(any(item.startswith("DESKWAIFU_TEST_TIMEOUT=")
                                    for item in properties["ENVIRONMENT_MODIFICATION"]))
                self.assertCountEqual(properties["LABELS"], ["architecture", "regression"])
                self.assertFalse(properties.get("RUN_SERIAL", False))
                self.assertNotIn("RESOURCE_LOCK", properties)

    def test_missing_or_failed_case_keeps_the_gate_red(self):
        report = ET.Element("testsuite")
        for name in self.expected:
            ET.SubElement(report, "testcase", name=name)
        for name in self.expected:
            with self.subTest(test=name):
                pattern = "^" + re.escape(name) + "$"
                missing = {"tests": [test for test in self.registry["tests"] if test["name"] != name]}
                summary = gate.evaluate(missing, report, self.policy, pattern)
                self.assertIn(f"required test not registered: {name}", summary["errors"])
                failed = ET.fromstring(f'<testsuite><testcase name="{name}"><failure/></testcase></testsuite>')
                summary = gate.evaluate(self.registry, failed, self.policy, pattern)
                self.assertIn(f"test failed: {name}", summary["errors"])


if __name__ == "__main__":
    unittest.main()
