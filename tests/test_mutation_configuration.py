import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mutations", ROOT / "scripts/check-test-mutations.py")
mutations = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mutations)


class MutationConfigurationTest(unittest.TestCase):
    def fixture(self, root, body):
        source, build = root / "source", root / "parent"
        source.mkdir()
        build.mkdir()
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(mutation_fixture NONE)\n' + body)
        (build / "CMakeCache.txt").write_text(
            "SDK_ROOT_PATH:PATH=/unused/sdk\nGLEW_PATH:PATH=/unused/glew\n"
            "DESKWAIFU_BUILD_OCR:BOOL=OFF\n")
        return source, build

    def invoke(self, source, build):
        with patch.object(mutations, "ROOT", source), patch.object(mutations, "MUTATIONS", []), \
                patch.object(sys, "argv", ["check-test-mutations.py", str(build)]):
            return mutations.main()

    def test_child_configure_does_not_enable_unused_native_ocr(self):
        with tempfile.TemporaryDirectory(prefix="mutation-config-") as directory:
            source, build = self.fixture(Path(directory),
                'option(DESKWAIFU_BUILD_OCR "Native OCR" ON)\n'
                'if(DESKWAIFU_BUILD_OCR)\n'
                '  message(FATAL_ERROR "Unexpected native OCR dependency in mutation build")\n'
                'endif()\n')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(self.invoke(source, build), 0)
            logs = list((build / "test-logs/mutations").glob("run-*/configure.log"))
            self.assertEqual(len(logs), 1)
            self.assertIn("Configuring done", logs[0].read_text())

    def test_configure_failure_surfaces_cmake_reason_and_retains_log(self):
        with tempfile.TemporaryDirectory(prefix="mutation-diagnostic-") as directory:
            source, build = self.fixture(Path(directory), 'message(FATAL_ERROR "fixture diagnostic sentinel")\n')
            with self.assertRaises(SystemExit) as error:
                self.invoke(source, build)
            self.assertIn("fixture diagnostic sentinel", str(error.exception))
            logs = list((build / "test-logs/mutations").glob("run-*/configure.log"))
            self.assertEqual(len(logs), 1)
            self.assertIn("fixture diagnostic sentinel", logs[0].read_text())


if __name__ == "__main__":
    unittest.main()
