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
        with tempfile.TemporaryDirectory(prefix="dw-c-boundary-") as directory:
            root = self.fixture(directory, sources="main.c")
            (root / "src/plugins/a/main.c").write_text('#include "../b/private/Foreign.h"\nint main(void) { return 0; }\n')
            (root / "src/plugins/b/private/Foreign.h").write_text("typedef int Foreign;\n")
            configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"))
            self.assertEqual(configured.returncode, 0, configured.stdout)
            result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("Ownership violation", result.stdout)

    def test_precompiled_headers_cannot_be_reused_across_owners(self):
        for foreign in (False, True):
            with self.subTest(foreign=foreign), tempfile.TemporaryDirectory(prefix="dw-pch-boundary-") as directory:
                root = self.fixture(directory, body="int owned(); int main() { return owned() == 7 ? 0 : 1; }\n")
                directory = root / ("src/plugins/b" if foreign else "src/plugins/a")
                with (directory / "CMakeLists.txt").open("a") as file:
                    file.write('\nadd_library(pch_seed STATIC pch.cpp)\ntarget_precompile_headers(pch_seed PRIVATE private/Pch.hpp)\n')
                (directory / "pch.cpp").write_text("int pch_seed() { return 0; }\n")
                (directory / "private/Pch.hpp").write_text("inline int private_value() { return 7; }\n")
                with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                    file.write('\nadd_library(own_impl STATIC owned.cpp)\ntarget_precompile_headers(own_impl REUSE_FROM pch_seed)\ntarget_link_libraries(a PRIVATE own_impl)\n')
                (root / "src/plugins/a/owned.cpp").write_text("int owned() { return private_value(); }\n")
                configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja")
                if foreign:
                    self.assertNotEqual(configured.returncode, 0, configured.stdout)
                    self.assertIn("cannot reuse precompiled headers", configured.stdout)
                else:
                    self.assertEqual(configured.returncode, 0, configured.stdout)
                    result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
                    self.assertEqual(result.returncode, 0, result.stdout)
                    executed = self.run_command(str(root / "build/src/plugins/a/a"))
                    self.assertEqual(executed.returncode, 0, executed.stdout)

    def test_link_artifacts_do_not_depend_on_output_directory_or_argument_spelling(self):
        for spelling in ("absolute", "wl", "xlinker", "response", "wl-response", "search", "xlinker-search", "object"):
            with self.subTest(spelling=spelling), tempfile.TemporaryDirectory(prefix="dw-artifact-boundary-") as directory:
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
                    "xlinker-search": 'target_link_options(a PRIVATE "SHELL:-Xlinker -L -Xlinker ${CMAKE_BINARY_DIR}/lib")\ntarget_link_libraries(a PRIVATE -lforeign_impl)',
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

    def test_static_archives_cannot_absorb_foreign_objects(self):
        for generator in ("Ninja", "Unix Makefiles"):
            for source in ("$<TARGET_OBJECTS:foreign_impl>", "$<$<BOOL:1>:$<TARGET_OBJECTS:foreign_impl>>",
                           "${CMAKE_BINARY_DIR}/src/plugins/b/CMakeFiles/foreign_impl.dir/private/foreign.cpp.o"):
                with self.subTest(generator=generator, source=source), tempfile.TemporaryDirectory(prefix="dw-archive-boundary-") as directory:
                    root = self.fixture(directory, body="int foreign(); int main() { return foreign(); }\n")
                    (root / "src/plugins/b/CMakeLists.txt").write_text("add_library(foreign_impl OBJECT private/foreign.cpp)\n")
                    with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                        file.write(f'\nadd_library(owned STATIC owned.cpp "{source}")\nadd_dependencies(owned foreign_impl)\ntarget_link_libraries(a PRIVATE owned)\n')
                        if source.startswith("${CMAKE_BINARY_DIR}"):
                            file.write(f'set_source_files_properties("{source}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)\n')
                    (root / "src/plugins/a/owned.cpp").write_text("int owned() { return 0; }\n")
                    configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"), "-G", generator)
                    self.assertEqual(configured.returncode, 0, configured.stdout)
                    result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn("cannot link plugin:b", result.stdout)
                    self.assertFalse((root / "build/src/plugins/a/libowned.a").exists())

    def test_library_search_respects_static_dynamic_selection(self):
        for foreign_kind in ("STATIC", "SHARED"):
            for static in (False, True):
                for state_stack in (False, True):
                    with self.subTest(foreign_kind=foreign_kind, static=static, state_stack=state_stack), tempfile.TemporaryDirectory(prefix="dw-library-mode-") as directory:
                        root = self.fixture(directory, body="int foreign(); int main() { return foreign(); }\n")
                        own_kind = "SHARED" if foreign_kind == "STATIC" else "STATIC"
                        (root / "src/plugins/b/CMakeLists.txt").write_text(f'''add_library(foreign_impl {foreign_kind} private/foreign.cpp)
set_target_properties(foreign_impl PROPERTIES OUTPUT_NAME mixed ARCHIVE_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib" LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib")
''')
                        (root / "src/plugins/b/private/foreign.cpp").write_text('__attribute__((visibility("default"))) int foreign() { return 3; }\n')
                        mode = "-Bstatic" if static else "-Bdynamic"
                        flags = f'"-Wl,--push-state,{mode}" -lmixed -Wl,--pop-state' if state_stack else f'"-Wl,{mode}" -lmixed -Wl,-Bdynamic'
                        with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                            file.write(f'''\nadd_library(own_impl {own_kind} owned.cpp)
set_target_properties(own_impl PROPERTIES OUTPUT_NAME mixed ARCHIVE_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib" LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib")
add_dependencies(a own_impl foreign_impl)
target_link_directories(a PRIVATE "${{CMAKE_BINARY_DIR}}/lib")
target_link_libraries(a PRIVATE {flags})
''')
                        (root / "src/plugins/a/owned.cpp").write_text('__attribute__((visibility("default"))) int foreign() { return 0; }\n')
                        configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja")
                        self.assertEqual(configured.returncode, 0, configured.stdout)
                        result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
                        forbidden = static == (foreign_kind == "STATIC")
                        if forbidden:
                            self.assertNotEqual(result.returncode, 0, result.stdout)
                            self.assertIn("cannot link plugin:b", result.stdout)
                        else:
                            self.assertEqual(result.returncode, 0, result.stdout)
                            executed = self.run_command(str(root / "build/src/plugins/a/a"))
                            self.assertEqual(executed.returncode, 0, executed.stdout)

    def test_forwarded_static_is_scoped_not_global(self):
        for foreign_kind in ("STATIC", "SHARED"):
            for alias in ("-static", "--static"):
                for wrapper in ("wl", "xlinker"):
                    with self.subTest(foreign_kind=foreign_kind, alias=alias, wrapper=wrapper), tempfile.TemporaryDirectory(prefix="dw-scoped-static-") as directory:
                        root = self.fixture(directory, body="int foreign(); int pinned(); int main() { return foreign() + pinned(); }\n")
                        own_kind = "SHARED" if foreign_kind == "STATIC" else "STATIC"
                        (root / "src/plugins/b/CMakeLists.txt").write_text(f'''add_library(foreign_impl {foreign_kind} private/foreign.cpp)
set_target_properties(foreign_impl PROPERTIES OUTPUT_NAME mixed ARCHIVE_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib" LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib")
''')
                        (root / "src/plugins/b/private/foreign.cpp").write_text('__attribute__((visibility("default"))) int foreign() { return 3; }\n')
                        flags = f'"-Wl,--push-state,{alias}" -lpinned -Wl,--pop-state -lmixed' if wrapper == "wl" else f'"-Xlinker" "--push-state" "-Xlinker" "{alias}" -lpinned "-Xlinker" "--pop-state" -lmixed'
                        with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                            file.write(f'''\nadd_library(own_impl {own_kind} owned.cpp)
add_library(pinned STATIC pinned.cpp)
set_target_properties(own_impl pinned PROPERTIES OUTPUT_NAME mixed ARCHIVE_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib" LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/lib")
set_target_properties(pinned PROPERTIES OUTPUT_NAME pinned)
add_dependencies(a own_impl foreign_impl pinned)
target_link_directories(a PRIVATE "${{CMAKE_BINARY_DIR}}/lib")
target_link_libraries(a PRIVATE {flags})
''')
                        (root / "src/plugins/a/owned.cpp").write_text('__attribute__((visibility("default"))) int foreign() { return 0; }\n')
                        (root / "src/plugins/a/pinned.cpp").write_text("int pinned() { return 0; }\n")
                        configured = self.run_command("cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja")
                        self.assertEqual(configured.returncode, 0, configured.stdout)
                        result = self.run_command("cmake", "--build", str(root / "build"), "--target", "a")
                        if foreign_kind == "SHARED":
                            self.assertNotEqual(result.returncode, 0, result.stdout)
                            self.assertIn("cannot link plugin:b", result.stdout)
                        else:
                            self.assertEqual(result.returncode, 0, result.stdout)
                            executed = self.run_command(str(root / "build/src/plugins/a/a"))
                            self.assertEqual(executed.returncode, 0, executed.stdout)

    def test_renamed_vendor_directory_has_consistent_ownership(self):
        with tempfile.TemporaryDirectory(prefix="dw-vendor-boundary-") as directory:
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
            with self.subTest(language=language), tempfile.TemporaryDirectory(prefix="dw-launcher-boundary-") as directory:
                root = self.fixture(directory, sources=source)
                (root / "src/plugins/a" / source).write_text("int main(void) { return 0; }\n")
                wrapper = root / "linker.sh"
                wrapper.write_text('#!/bin/sh\ntouch "' + str(root / "linked") + '"\nexec "$@"\n')
                wrapper.chmod(0o700)
                archive_wrapper = root / "archive.sh"
                archive_wrapper.write_text('#!/bin/sh\ntouch "' + str(root / "archived") + '"\nexec "$@"\n')
                archive_wrapper.chmod(0o700)
                with (root / "src/plugins/a/CMakeLists.txt").open("a") as file:
                    file.write(f'''\nset_property(TARGET a PROPERTY {language}_LINKER_LAUNCHER "{wrapper}")
add_library(own_objects OBJECT owned.c)
add_library(owned STATIC $<TARGET_OBJECTS:own_objects>)
set_property(TARGET owned PROPERTY RULE_LAUNCH_LINK "'{archive_wrapper}'")
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
                self.assertTrue((root / "archived").exists())

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
