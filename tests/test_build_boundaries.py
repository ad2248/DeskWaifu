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
            "CMakeLists.txt": """cmake_minimum_required(VERSION 3.21)
project(ownership_fixture LANGUAGES C CXX)
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
            "platform/tooling/check_dependencies.py": (ROOT / "platform/tooling/check_dependencies.py").read_text(),
            "platform/tooling/check_link_dependencies.py": (ROOT / "platform/tooling/check_link_dependencies.py").read_text(),
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

    def test_c_include_ownership(self):
        with tempfile.TemporaryDirectory(prefix="dw-c-boundary-", dir="/tmp/opencode") as directory:
            root = self.fixture(directory, sources="main.c")
            (root / "src/plugins/a/main.c").write_text('#include "../b/private/Foreign.h"\nint main(void) { return 0; }\n')
            (root / "src/plugins/b/private/Foreign.h").write_text("typedef int Foreign;\n")
            configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
            self.assertEqual(configured.returncode, 0, configured.stdout)
            result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("Ownership violation", result.stdout)

    def test_link_artifacts_do_not_depend_on_output_directory_or_argument_spelling(self):
        for spelling in ("absolute", "wl", "xlinker", "response", "wl-response", "search", "object"):
            with self.subTest(spelling=spelling), tempfile.TemporaryDirectory(prefix="dw-artifact-boundary-", dir="/tmp/opencode") as directory:
                root = self.fixture(directory, body="int foreign(); int main() { return foreign(); }\n")
                (root / "src/plugins/b/CMakeLists.txt").write_text('''add_library(foreign_impl STATIC private/foreign.cpp)
set_target_properties(foreign_impl PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
''')
                archive = "${CMAKE_BINARY_DIR}/lib/libforeign_impl.a"
                link = {
                    "absolute": f'target_link_libraries(a PRIVATE "{archive}")',
                    "wl": f'target_link_libraries(a PRIVATE "-Wl,{archive}")',
                    "xlinker": f'target_link_options(a PRIVATE -Xlinker "{archive}")',
                    "response": 'target_link_options(a PRIVATE "@${CMAKE_BINARY_DIR}/foreign.rsp")',
                    "wl-response": 'target_link_options(a PRIVATE "-Wl,@${CMAKE_BINARY_DIR}/foreign.rsp")',
                    "search": 'target_link_directories(a PRIVATE "${CMAKE_BINARY_DIR}/lib")\ntarget_link_libraries(a PRIVATE -lforeign_impl)',
                    "object": 'target_link_libraries(a PRIVATE "-Wl,${CMAKE_BINARY_DIR}/src/plugins/b/CMakeFiles/foreign_impl.dir/private/foreign.cpp.o")',
                }[spelling]
                with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                    file.write(f'\nadd_dependencies(a foreign_impl)\nfile(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/foreign.rsp" CONTENT "{archive}")\n{link}\n')
                configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja")
                self.assertEqual(configured.returncode, 0, configured.stdout)
                result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("cannot link", result.stdout)
                self.assertFalse((root / "build/src/plugins/a/a").exists())

    def test_renamed_vendor_directory_has_consistent_ownership(self):
        with tempfile.TemporaryDirectory(prefix="dw-vendor-boundary-", dir="/tmp/opencode") as directory:
            root = self.fixture(directory, body='#include "Vendor.hpp"\nint main() { return vendor(); }\n')
            vendor = root / "CubismSdkForNative-next"
            vendor.mkdir()
            (vendor / "Vendor.hpp").write_text("inline int vendor() { return 0; }\n")
            with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                file.write('\ntarget_include_directories(a PRIVATE "${PROJECT_SOURCE_DIR}/CubismSdkForNative-next")\n')
            configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
            self.assertEqual(configured.returncode, 0, configured.stdout)
            result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_same_owner_archives_and_existing_launchers_are_allowed(self):
        for language, source in (("C", "main.c"), ("CXX", "main.cpp")):
            with self.subTest(language=language), tempfile.TemporaryDirectory(prefix="dw-launcher-boundary-", dir="/tmp/opencode") as directory:
                root = self.fixture(directory, sources=source)
                (root / "src/plugins/a" / source).write_text("int main(void) { return 0; }\n")
                wrapper = root / "linker.sh"
                wrapper.write_text('#!/bin/sh\ntouch "' + str(root / "linked") + '"\nexec "$@"\n')
                wrapper.chmod(0o700)
                with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                    file.write(f'''\nset_property(TARGET a PROPERTY {language}_LINKER_LAUNCHER "{wrapper}")
add_library(owned STATIC owned.c)
set_target_properties(owned PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib")
target_link_libraries(a PRIVATE "${{CMAKE_BINARY_DIR}}/lib/libowned.a")
add_dependencies(a owned)
''')
                (root / "src/plugins/a/owned.c").write_text("int owned(void) { return 0; }\n")
                configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
                self.assertEqual(configured.returncode, 0, configured.stdout)
                result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertTrue((root / "linked").exists())

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
