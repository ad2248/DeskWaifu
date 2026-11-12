"""Exercise the real OCR build/install rules with dependency and source stubs.

These fixtures test CMake integration without real Qt/OpenCV/ORT libraries or OCR models;
they do not substitute for compiling the native inference implementation.
"""

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
NATIVE = Path("plugins/perception/native")


class OcrBuildTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dw-ocr-build-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "source"
        self.build = self.directory / "build"
        self.prefix = self.directory / "installed"
        self.dependencies = self.directory / "dependencies"

    def write(self, path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def run_command(self, *arguments, success=True):
        result = subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def fixture(self, yaml_target="yaml-cpp::yaml-cpp"):
        native = self.source / NATIVE
        for name in ("CMakeLists.txt", "LICENSE-PaddleOCR", "NOTICE", "README.md", "RESULTS.md"):
            self.write(native / name, (ROOT / NATIVE / name).read_text())
        # The real module uses AUTOMOC; these dependency-free stubs do not.
        with (native / "CMakeLists.txt").open("a") as file:
            file.write('\nset_target_properties(deskwaifu-ocr PROPERTIES AUTOMOC OFF)\n')
        with (native / "CMakeLists.txt").open("a") as file:
            file.write('\nfile(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/helper-links.txt" '
                       'CONTENT "$<TARGET_PROPERTY:deskwaifu-ocr,LINK_LIBRARIES>")\n')
        self.write(native / "OcrModule.cpp", "int module_fixture() { return 0; }\n")
        self.write(native / "OcrEngine.cpp", "// Inference implementation is outside this build-rule fixture.\n")
        self.write(native / "OcrEngine.hpp", "#pragma once\n")
        for relative in ("CMakeLists.txt", "cmake/PluginBoundaries.cmake",
                         "platform/tooling/check_dependencies.py",
                         "platform/tooling/check_link_dependencies.py"):
            self.write(self.source / relative, (ROOT / relative).read_text())
        self.write(self.source / "sdk/CMakeLists.txt", "")
        self.write(self.source / "plugins/CMakeLists.txt", "")
        self.write(self.source / "platform/CMakeLists.txt", """add_executable(deskwaifu main.cpp)
install(TARGETS deskwaifu RUNTIME DESTINATION bin COMPONENT DeskWaifu)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/app-links.txt" CONTENT "$<TARGET_PROPERTY:deskwaifu,LINK_LIBRARIES>")
""")
        self.write(self.source / "platform/main.cpp", "int main() { return 0; }\n")
        self.write(self.dependencies / "Qt6/Qt6Config.cmake", """set(Qt6_FOUND TRUE)
foreach(component Core Gui)
  add_library(Qt6::${component} INTERFACE IMPORTED)
endforeach()
""")
        self.write(self.dependencies / "Qt6/Qt6ConfigVersion.cmake", """set(PACKAGE_VERSION 6.5.0)
set(PACKAGE_VERSION_COMPATIBLE TRUE)
""")
        self.write(self.dependencies / "OpenCV/OpenCVConfig.cmake", """set(OpenCV_FOUND TRUE)
set(OpenCV_LIBS opencv_core opencv_imgproc)
add_library(opencv_core INTERFACE IMPORTED)
add_library(opencv_imgproc INTERFACE IMPORTED)
""")
        self.write(self.dependencies / "yaml-cpp/yaml-cpp-config.cmake",
                   f"add_library({yaml_target} INTERFACE IMPORTED)\n")
        self.write(self.dependencies / "ort/include/onnxruntime/onnxruntime_cxx_api.h", "// System header stub\n")
        self.write(self.dependencies / "onnxruntime/onnxruntimeConfig.cmake", """add_library(onnxruntime::onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../ort/lib/libonnxruntime.so"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../ort/include/onnxruntime")
""")
        library = self.dependencies / "ort/lib/libonnxruntime.so"
        library.parent.mkdir(parents=True)
        stub = self.dependencies / "ort/stub.cpp"
        self.write(stub, "int ort_fixture() { return 0; }\n")
        self.run_command("c++", "-shared", "-fPIC", str(stub), "-o", str(library))

    def configure(self, standalone=False, ocr=True, success=True):
        arguments = ["cmake", "-S", str(self.source / NATIVE if standalone else self.source),
                     "-B", str(self.build), "-G", "Ninja",
                     f"-DCMAKE_INSTALL_PREFIX={self.prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_TESTING=OFF"]
        if ocr:
            arguments += [f"-DCMAKE_PREFIX_PATH={self.dependencies}"]
        if not standalone:
            arguments += ["-DBUILD_TESTING=OFF", "-DDESKWAIFU_BUILD_CONTROL=OFF",
                          "-DDESKWAIFU_BUILD_GUI_PLUGIN_SDK=OFF",
                          "-DDESKWAIFU_BUILD_RUST_RUNTIME=OFF"]
            if not ocr:
                arguments.append("-DDESKWAIFU_BUILD_OCR=OFF")
        return self.run_command(*arguments, success=success)

    def assert_model_free_install(self):
        paths = {str(path.relative_to(self.prefix)) for path in self.prefix.rglob("*") if path.is_file()}
        self.assertIn("lib/deskwaifu/ocr/deskwaifu-ocr.so", paths)
        self.assertNotIn("libexec/deskwaifu-ocr", paths)
        self.assertFalse(any("libonnxruntime" in path or "/onnxruntime/" in path for path in paths), paths)
        models = self.prefix / "share/deskwaifu/ocr/models"
        self.assertTrue(models.is_dir())
        self.assertEqual(list(models.iterdir()), [])
        self.assertFalse(any(path.endswith((".onnx", ".json", ".yml")) for path in paths), paths)

    def test_default_root_build_includes_optional_module_without_models(self):
        self.fixture()
        self.configure()
        self.assertIn("DESKWAIFU_BUILD_OCR:BOOL=ON", (self.build / "CMakeCache.txt").read_text())
        self.run_command("cmake", "--build", str(self.build))
        self.assertTrue((self.build / NATIVE / "deskwaifu-ocr.so").is_file())
        self.assertTrue((self.build / "bin/deskwaifu").is_file())
        self.assertEqual((self.build / "app-links.txt").read_text(), "")
        self.assertIn("yaml-cpp::yaml-cpp", (self.build / "helper-links.txt").read_text())
        self.assertIn("onnxruntime::onnxruntime", (self.build / "helper-links.txt").read_text())
        self.assertFalse((self.build / "CPackConfig.cmake").exists())
        self.run_command("cmake", "--install", str(self.build), "--component", "DeskWaifu")
        self.assertTrue((self.prefix / "bin/deskwaifu").is_file())
        self.assertFalse((self.prefix / "lib/deskwaifu/ocr/deskwaifu-ocr.so").exists())
        self.run_command("cmake", "--install", str(self.build), "--component", "DeskWaifuOCR")
        self.assert_model_free_install()

    def test_standalone_build_and_cpack_need_no_models(self):
        self.fixture(yaml_target="yaml-cpp")
        self.configure(standalone=True)
        self.assertIn("yaml-cpp", (self.build / "helper-links.txt").read_text())
        self.run_command("cmake", "--build", str(self.build))
        self.run_command("cmake", "--install", str(self.build))
        self.assert_model_free_install()
        packages = self.directory / "packages"
        self.run_command("cpack", "--config", str(self.build / "CPackConfig.cmake"), "-B", str(packages))
        archive, = packages.glob("deskwaifu-ocr-*.tar.xz")
        with tarfile.open(archive) as package:
            names = package.getnames()
            models = [member for member in package.getmembers()
                      if member.name.rstrip("/").endswith("share/deskwaifu/ocr/models")]
            self.assertEqual(len(models), 1, names)
            self.assertTrue(models[0].isdir())
        self.assertTrue(any(name.endswith("lib/deskwaifu/ocr/deskwaifu-ocr.so") for name in names), names)
        self.assertFalse(any("libonnxruntime" in name or "/onnxruntime/" in name for name in names), names)
        self.assertFalse(any(name.endswith("libexec/deskwaifu-ocr") for name in names), names)
        self.assertFalse(any(name.endswith((".onnx", ".json", ".yml")) for name in names), names)

    def test_explicit_ocr_off_needs_no_native_dependencies(self):
        self.fixture()
        shutil.rmtree(self.dependencies)
        self.configure(ocr=False)
        self.run_command("cmake", "--build", str(self.build))
        self.assertTrue((self.build / "bin/deskwaifu").is_file())
        self.assertFalse((self.build / NATIVE / "deskwaifu-ocr.so").exists())

    def test_missing_system_onnxruntime_is_reported(self):
        self.fixture()
        # Disable discovery explicitly so the host's installed package cannot
        # mask a missing dependency in the fixture.
        result = self.run_command("cmake", "-S", str(self.source / NATIVE), "-B", str(self.build),
                                  f"-DCMAKE_PREFIX_PATH={self.dependencies}",
                                  "-DCMAKE_DISABLE_FIND_PACKAGE_onnxruntime=ON", success=False)
        self.assertIn("onnxruntime", result.stdout)
        self.assertIn("REQUIRED", result.stdout)

    def test_release_patches_historical_sdk_recipe_to_system_ort(self):
        self.fixture()
        native = self.source / NATIVE / "CMakeLists.txt"
        native.write_text((Path(__file__).parent / "fixtures/ocr-sdk-CMakeLists.txt").read_text() +
                          '\nset_target_properties(deskwaifu-ocr PROPERTIES AUTOMOC OFF)\n')
        (self.directory / "deskwaifu").symlink_to(self.source, target_is_directory=True)
        for name in ("deskwaifu.tar.gz", "private-deps.tar.gz", "release-tools.tar.gz"):
            (self.directory / name).write_bytes(b"checksum fixture")
        self.run_command(sys.executable, "-B", str(ROOT / "scripts/release-packaging.py"),
                         "prepare", str(self.directory), "0.2.0", "a" * 40)
        self.run_command("bash", "-euc", 'source "$1"; srcdir=$2; prepare', "bash",
                         str(self.directory / "PKGBUILD"), str(self.directory))
        self.assertNotIn("ONNXRUNTIME_ROOT", native.read_text())
        self.assertNotIn("$ORIGIN", native.read_text())
        self.configure(standalone=True)
        self.run_command("cmake", "--build", str(self.build))
        self.run_command("cmake", "--install", str(self.build))
        self.assert_model_free_install()

    def test_release_leaves_current_system_recipe_unchanged(self):
        self.fixture()
        native = self.source / NATIVE / "CMakeLists.txt"
        original = native.read_text()
        (self.directory / "deskwaifu").symlink_to(self.source, target_is_directory=True)
        self.run_command("bash", "-euc", 'source "$1"; srcdir=$2; prepare', "bash",
                         str(ROOT / "packaging/release/PKGBUILD.in"), str(self.directory))
        self.assertEqual(native.read_text(), original)

    def test_release_prepare_patches_both_tag_layouts(self):
        historical = (Path(__file__).parent / "fixtures/ocr-sdk-CMakeLists.txt").read_text()
        for layout in ("plugins", "src/plugins"):
            with self.subTest(layout=layout), tempfile.TemporaryDirectory() as directory:
                source = Path(directory)
                native = source / "deskwaifu" / layout / "perception/native/CMakeLists.txt"
                self.write(native, historical)
                shutil.copyfile(ROOT / "packaging/release/system-onnxruntime.patch",
                                source / "system-onnxruntime.patch")
                self.run_command("bash", "-euc", 'source "$1"; srcdir="$2"; prepare', "bash",
                                 str(ROOT / "packaging/release/PKGBUILD.in"), str(source))
                self.assertIn("find_package(onnxruntime CONFIG REQUIRED)", native.read_text())
                self.assertNotIn("ONNXRUNTIME_ROOT", native.read_text())

    def test_release_build_selects_checked_out_tag_layout(self):
        for layouts in (("plugins",), ("src/plugins",), ("plugins", "src/plugins"), ()):
            with self.subTest(layouts=layouts), tempfile.TemporaryDirectory() as directory:
                source = Path(directory)
                stub = "cmake_minimum_required(VERSION 3.25)\nproject(release_fixture NONE)\n"
                self.write(source / "deskwaifu/CMakeLists.txt", stub)
                # This fixture checks source selection. Runtime registry export
                # is tested separately with real CTest registrations.
                self.write(source / "release-tools/scripts/prepare-installed-tests.py",
                           "from pathlib import Path\nimport sys\n"
                           "(Path(sys.argv[1]) / 'export-called').touch()\n")
                for layout in layouts:
                    self.write(source / "deskwaifu" / layout / "perception/native/CMakeLists.txt", stub)
                result = subprocess.run([
                    "bash", "-euc", 'source "$1"; srcdir="$2"; prepare; build', "bash",
                    str(ROOT / "packaging/release/PKGBUILD.in"), str(source),
                ], capture_output=True, text=True)
                if layouts:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    cache = (source / "build-ocr/CMakeCache.txt").read_text()
                    expected = source / "deskwaifu" / layouts[0] / "perception/native"
                    self.assertIn(f"CMAKE_HOME_DIRECTORY:INTERNAL={expected}\n", cache)
                    self.assertTrue((source / "build-main/export-called").is_file())
                    self.assertTrue((source / "build-ocr/export-called").is_file())
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("OCR source CMakeLists.txt not found", result.stderr)
                    self.assertFalse((source / "build-main").exists())

    def test_legacy_ci_and_aur_explicitly_disable_native_dependencies(self):
        workflow = (ROOT / ".gitea/workflows/ci.yml").read_text()
        configurations = re.findall(r"cmake -S \..*?(?<!\\)\n", workflow, re.DOTALL)
        self.assertTrue(configurations)
        for command in configurations:
            self.assertIn("-DDESKWAIFU_BUILD_OCR=OFF", command)
        self.assertIn("-DDESKWAIFU_BUILD_OCR=OFF", (ROOT / "packaging/arch/deskwaifu-git/PKGBUILD").read_text())


if __name__ == "__main__":
    unittest.main()
