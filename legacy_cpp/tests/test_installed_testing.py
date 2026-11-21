import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


installation = load("test-installed-packages")


class InstalledTestingTest(unittest.TestCase):
    def test_migrated_ocr_fixture_is_preserved_for_installed_smoke_tests(self):
        self.assertTrue(installation.OCR_MODEL_TEST.is_file())
        relative = installation.OCR_MODEL_TEST.relative_to(ROOT).as_posix()
        self.assertIn(relative, (ROOT / "scripts/release-source.py").read_text())

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

if __name__ == "__main__":
    unittest.main()
