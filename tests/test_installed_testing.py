import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


exporter = load("prepare-installed-tests")
runner = load("run-installed-tests")
installation = load("test-installed-packages")


class InstalledTestingTest(unittest.TestCase):
    def test_migrated_ocr_fixture_is_preserved_for_installed_smoke_tests(self):
        self.assertTrue(installation.OCR_MODEL_TEST.is_file())
        relative = installation.OCR_MODEL_TEST.relative_to(ROOT).as_posix()
        self.assertIn(relative, (ROOT / "scripts/release-source.py").read_text())

    def test_export_relocates_runtime_targets_and_keeps_source_checks_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            build = Path(directory) / "build"
            source.mkdir()
            build.mkdir()
            (source / "BackendCapabilitiesTest.py").write_text("# portable\n")
            (build / "test-targets.json").write_text(json.dumps([
                {"build": str(build / "bin/unit-test"), "installed": "/usr/lib/deskwaifu/tests/targets/unit/unit-test"},
                {"build": str(build / "bin/deskwaifu"), "installed": "/usr/bin/deskwaifu"}, None]))
            registry = {"tests": [
                {"name": "unit", "command": [str(build / "bin/unit-test")]},
                {"name": "integration", "command": ["python3", str(source / "BackendCapabilitiesTest.py"), str(build / "bin/deskwaifu")]},
                {"name": "source-contract", "command": ["python3", str(source / "check-source.py")]},
                {"name": "compile-contract", "command": ["cmake", str(build / "compiler-fixture")]},
            ]}
            with patch.object(exporter.subprocess, "check_output", return_value=json.dumps(registry)):
                exporter.prepare(build, source, ROOT / "scripts")
            result = json.loads((build / "installed-tests.json").read_text())
            self.assertEqual([test["name"] for test in result["tests"]], ["unit", "integration"])
            self.assertEqual(result["source_gates"], ["source-contract", "compile-contract"])
            self.assertNotIn(str(source), json.dumps(result))
            self.assertNotIn(str(build), json.dumps(result))
            self.assertIn("/usr/bin/deskwaifu", result["tests"][1]["command"])

    def test_empty_export_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "test-targets.json").write_text("[]")
            with patch.object(exporter.subprocess, "check_output", return_value='{"tests": []}'):
                with self.assertRaisesRegex(ValueError, "No installable"):
                    exporter.prepare(root, root, ROOT / "scripts")

    def test_installed_runner_rejects_failed_and_unexpected_skipped_tests(self):
        for code, output, expected in ((0, "passed", False), (1, "failure", True),
                                       (77, "skip", True), (0, "SKIP : testCase() reason\n", True),
                                       (0, "OK (skipped=1)\n", True)):
            with self.subTest(code=code, output=output), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "installed-tests.json").write_text(json.dumps({"tests": [
                    {"name": "unit", "command": ["unit-test"], "environment": [], "timeout": 1, "skip": 77}],
                    "optional_processes": {}, "optional_cases": {}}))
                with patch.object(runner, "__file__", str(root / "run.py")), \
                        patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], code, output)), \
                        patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(runner.main(), expected)

    def test_four_install_stages_are_fixed_and_release_never_runs_them(self):
        self.assertEqual(installation.PACKAGES, ("deskwaifu-bin", "deskwaifu-ocr-bin", "deskwaifu-tests", "deskwaifu-ocr-tests"))
        workflow = (ROOT / ".gitea/workflows/release.yml").read_text()
        self.assertNotIn("test-installed-packages.py", workflow)
        self.assertNotIn("ctest", workflow)
        template = (ROOT / "packaging/release/PKGBUILD.in").read_text()
        self.assertEqual(template.count("-DBUILD_TESTING=ON"), 2)
        self.assertIn("options=('!debug' '!lto')", template)
        self.assertIn("'noto-fonts'", template)
        self.assertNotIn("check()", template)

    def test_cmake_relocates_static_fixture_definitions_and_test_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, build = root / "source", root / "build"
            (source / "testing").mkdir(parents=True)
            (source / "unit.cpp").write_text("int main() { return 0; }\n")
            (source / "testing/fixture.cpp").write_text("int fixture() { return 0; }\n")
            (source / "testing/CMakeLists.txt").write_text(
                'add_library(fixture MODULE fixture.cpp)\n'
                'add_library(support STATIC fixture.cpp)\n'
                'target_compile_definitions(support PRIVATE FIXTURE="$<TARGET_FILE:fixture>")\n')
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(installation_fixture LANGUAGES CXX)\n'
                'include(GNUInstallDirs)\nadd_subdirectory(testing)\n'
                'add_executable(unit-test unit.cpp)\n'
                f'set(DESKWAIFU_TEST_TOOLS "{ROOT}/scripts")\n'
                f'include("{ROOT}/cmake/InstallTests.cmake")\n'
                'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/definitions.txt" '
                'CONTENT "$<TARGET_PROPERTY:support,COMPILE_DEFINITIONS>")\n')
            subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                            "-DCMAKE_INSTALL_PREFIX=/usr", "-DCMAKE_INSTALL_LIBDIR=lib"],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertIn('/usr/lib/deskwaifu/tests/targets/fixture/', (build / "definitions.txt").read_text())
            mappings = json.loads((build / "test-targets.json").read_text())
            self.assertTrue(any(entry and '/targets/unit-test/' in entry["installed"] for entry in mappings))

    def test_proot_daemon_success_cannot_hide_test_failure_or_missing_result(self):
        for contents in (None, "1", "0"):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as directory:
                guest = installation.Root.__new__(installation.Root)
                guest.root = Path(directory)
                (guest.root / "tmp").mkdir()
                if contents is not None:
                    (guest.root / "tmp/exit-fixed").write_text(contents)
                with patch.object(installation.uuid, "uuid4") as token, \
                        patch.object(installation.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
                    token.return_value.hex = "fixed"
                    if contents == "0":
                        guest.run(["failing-test"])
                    else:
                        with self.assertRaisesRegex(RuntimeError, "failed or did not finish"):
                            guest.run(["failing-test"])


if __name__ == "__main__":
    unittest.main()
