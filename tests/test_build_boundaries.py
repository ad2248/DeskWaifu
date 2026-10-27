"""Exercise the real build firewall with an independent matrix of bad consumers.

The fixtures need no Qt/Cubism and cannot pass merely because a private header
has a missing dependency: each forbidden include is otherwise valid C++.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildBoundaries(unittest.TestCase):
    def fixture(self, directory, body="int main() { return 0; }", link="", sources="main.cpp"):
        root = Path(directory)
        files = {
            "CMakeLists.txt": """cmake_minimum_required(VERSION 3.19)
project(ownership_fixture LANGUAGES CXX)
include(cmake/PluginBoundaries.cmake)
add_subdirectory(sdk)
add_subdirectory(src/plugins/b)
add_subdirectory(src/plugins/a)
deskwaifu_validate_boundaries()
""",
            "sdk/CMakeLists.txt": "add_library(sdk INTERFACE)\n",
            "sdk/Public.hpp": "#pragma once\nstruct Public {};\n",
            "src/plugins/a/CMakeLists.txt": f"""add_executable(a {sources})
target_include_directories(a PRIVATE ${{PROJECT_SOURCE_DIR}}/sdk ${{CMAKE_CURRENT_SOURCE_DIR}}/private)
{link}
""",
            "src/plugins/a/main.cpp": body,
            "src/plugins/a/private/Owned.hpp": "#pragma once\nstruct Owned {};\n",
            "src/plugins/b/CMakeLists.txt": "add_library(b STATIC private/foreign.cpp)\n",
            "src/plugins/b/private/foreign.cpp": "int foreign() { return 3; }\n",
            "src/plugins/b/private/Foreign.hpp": "#pragma once\nstruct Foreign {};\n",
            "platform/Private.hpp": "#pragma once\nstruct PlatformPrivate {};\n",
            "cmake/PluginBoundaries.cmake": (ROOT / "cmake/PluginBoundaries.cmake").read_text(),
            "platform/build/check_dependencies.py": (ROOT / "platform/build/check_dependencies.py").read_text(),
        }
        for relative, text in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        return root

    @staticmethod
    def run_command(*arguments):
        return subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_compilation_ownership(self):
        with tempfile.TemporaryDirectory(prefix="dw-compile-boundary-") as directory:
            root = self.fixture(directory, sources="main.cpp ../b/private/foreign.cpp")
            result = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("cannot compile", result.stdout)

    def test_link_ownership(self):
        for linkage in ("PRIVATE b", "PRIVATE $<LINK_ONLY:b>", "INTERFACE b"):
            with self.subTest(linkage=linkage), tempfile.TemporaryDirectory(prefix="dw-link-boundary-") as directory:
                root = self.fixture(directory, link=f"target_link_libraries(a {linkage})")
                result = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("cannot link", result.stdout)

    def test_include_ownership(self):
        for spelling in ("relative", "absolute", "angle", "platform"):
            with self.subTest(spelling=spelling), tempfile.TemporaryDirectory(prefix="dw-include-boundary-") as directory:
                foreign = Path(directory) / "src/plugins/b/private/Foreign.hpp"
                include = {
                    "relative": '"../b/private/Foreign.hpp"',
                    "absolute": f'"{foreign}"',
                    "angle": f'<{foreign}>',
                    "platform": '"../../../platform/Private.hpp"',
                }[spelling]
                root = self.fixture(directory, body=f"#include {include}\nint main() {{ return 0; }}\n")
                configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
                self.assertEqual(configured.returncode, 0, configured.stdout)
                result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("Ownership violation", result.stdout)
                self.assertFalse((root / "build/src/plugins/a/CMakeFiles/a.dir/main.cpp.o").exists())

    def test_sdk_include_surface_does_not_expose_plugin_private_headers(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="dw-sdk-surface-") as directory:
            source = Path(directory) / "consumer.cpp"
            for header in ("processor/model/ModelProcessor.hpp", "processor/ipc/LAppIPC.hpp",
                           "processor/ocr/OcrClient.hpp", "app/LAppDelegate.hpp"):
                with self.subTest(header=header):
                    source.write_text(f'#include "{header}"\n')
                    result = self.run_command(compiler, "-fsyntax-only", "-I" + str(ROOT / "sdk"), str(source))
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn(header, result.stdout)

    def test_valid_same_owner_and_sdk_consumer_builds(self):
        with tempfile.TemporaryDirectory(prefix="dw-valid-boundary-") as directory:
            root = self.fixture(directory, body='#include "Owned.hpp"\n#include "Public.hpp"\nint main() { Owned a; Public b; return 0; }\n')
            configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
            self.assertEqual(configured.returncode, 0, configured.stdout)
            built = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
            self.assertEqual(built.returncode, 0, built.stdout)


if __name__ == "__main__":
    unittest.main()
