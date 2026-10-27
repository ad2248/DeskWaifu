"""Exercise the build firewall, including absolute includes and indirect links."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Boundaries(unittest.TestCase):
    def fixture(self, root, include='"own.hpp"', link="", launcher=""):
        files = {
            "CMakeLists.txt": f'''cmake_minimum_required(VERSION 3.21)
project(boundaries LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
{launcher}
include("{ROOT}/cmake/PluginBoundaries.cmake")
add_subdirectory(sdk)
add_subdirectory(src/plugins/a)
add_subdirectory(src/plugins/b)
deskwaifu_validate_boundaries()
''',
            "sdk/CMakeLists.txt": "add_library(sdk STATIC api.cpp)\n",
            "sdk/api.cpp": "int api() { return 1; }\n",
            "src/plugins/a/CMakeLists.txt": "add_library(a STATIC a.cpp)\nadd_library(alias_a ALIAS a)\n",
            "src/plugins/a/a.cpp": "int private_api() { return 2; }\n",
            "src/plugins/a/private.hpp": "inline int secret() { return 2; }\n",
            "src/plugins/b/CMakeLists.txt": f"add_library(b STATIC b.cpp)\n{link}\n",
            "src/plugins/b/own.hpp": "inline int own() { return 3; }\n",
            "src/plugins/b/b.cpp": f"#include {include}\nint b() {{ return 1; }}\n",
        }
        for relative, text in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        checker = root / "platform/tooling/check_dependencies.py"
        checker.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "platform/tooling/check_dependencies.py", checker)
        shutil.copyfile(ROOT / "platform/tooling/check_link_dependencies.py", checker.with_name("check_link_dependencies.py"))

    def build(self, root):
        configure = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja"],
                                   capture_output=True, text=True)
        if configure.returncode:
            return configure
        return subprocess.run(["cmake", "--build", str(root / "build")], capture_output=True, text=True)

    def test_own_headers_and_sdk_links_are_allowed(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            self.fixture(root, link="target_link_libraries(b PRIVATE sdk)")
            result = self.build(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_foreign_headers_fail_even_with_explicit_paths(self):
        for absolute in (False, True):
            with self.subTest(absolute=absolute), tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
                root = Path(directory)
                include = str(root / "src/plugins/a/private.hpp") if absolute else "../a/private.hpp"
                self.fixture(root, include=f'"{include}"')
                result = self.build(root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Ownership violation", result.stdout + result.stderr)

    def test_existing_launcher_is_preserved(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            wrapper = root / "launcher.sh"
            wrapper.write_text('#!/bin/sh\ntouch "' + str(root / "launched") + '"\nexec "$@"\n')
            wrapper.chmod(0o700)
            self.fixture(root, launcher=f'set(CMAKE_CXX_COMPILER_LAUNCHER "{wrapper}")')
            result = self.build(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((root / "launched").exists())

    def test_foreign_links_fail(self):
        for dependency in ("a", "alias_a", "$<BUILD_INTERFACE:a>", "indirect",
                           "${CMAKE_BINARY_DIR}/src/plugins/a/liba.a"):
            with self.subTest(dependency=dependency), tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
                root = Path(directory)
                link = ""
                if dependency == "indirect":
                    link = "add_library(indirect INTERFACE)\ntarget_link_libraries(indirect INTERFACE a)\n"
                self.fixture(root, link=link + f'target_link_libraries(b PRIVATE "{dependency}")')
                result = self.build(root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("cannot link", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
