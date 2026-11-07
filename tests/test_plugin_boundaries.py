"""Ownership rules for the SDK, platform and dynamically loaded domains."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PluginBoundaries(unittest.TestCase):
    def test_plugins_live_at_repository_root(self):
        self.assertFalse((ROOT / "src").exists())
        self.assertTrue((ROOT / "plugins/CMakeLists.txt").is_file())

    def test_root_tests_have_no_domain_scripts(self):
        for path in (ROOT / "tests").iterdir():
            with self.subTest(path=path.name):
                self.assertNotRegex(path.name, r"^(?:Check.*\.cmake|fake-input\.xml|"
                                    r"(?:qt_headless|hotkey|output)_smoke\.py|"
                                    r"test_(?:ctl|ocr_.*|qt_window_.*|kwin_.*)\.(?:py|js))$")

    def test_domain_script_entrypoints_are_local(self):
        for owner, scripts in {
            "desktop": ("test_qt_window_configuration.py", "CheckComponentDirection.cmake",
                        "test_kwin_cursor_script.js", "test_kwin_cursor_locale.py", "fake-input.xml"),
            "ipc": ("test_ctl.py", "CheckIpcTransportHeader.cmake"),
            "perception": ("test_ocr_build.py",),
            "speech": ("CheckNoSpeechConfiguration.cmake",),
        }.items():
            directory = ROOT / "plugins" / owner / "tests"
            cmake = (directory / "CMakeLists.txt").read_text()
            for script in scripts:
                with self.subTest(owner=owner, script=script):
                    self.assertTrue((directory / script).is_file())
                    self.assertIn(script, cmake)
        native = ROOT / "plugins/perception/native/CMakeLists.txt"
        self.assertIn("../tests/test_ocr_native_models.py", native.read_text())

    def test_shared_checks_follow_platform_support_directory(self):
        helper = "platform/tests/support/CheckPortableTemporaryDirectory.cmake"
        self.assertTrue((ROOT / helper).is_file())
        for entry in ("platform/tests/CMakeLists.txt", "plugins/desktop/tests/CMakeLists.txt"):
            text = (ROOT / entry).read_text()
            self.assertIn(helper, text)
            self.assertNotIn("platform/testing/", text)

    def test_host_has_no_speech_adapter(self):
        for relative in (
            "platform/main.cpp", "plugins/desktop/private/app/LAppDelegate.cpp",
            "plugins/desktop/private/app/LAppDelegate.hpp",
            "plugins/ipc/private/processor/command/CommandProcessor.hpp",
            "plugins/desktop/private/processor/interaction/InteractionProcessor.cpp",
            "plugins/desktop/private/processor/interaction/InteractionProcessor.hpp",
            "plugins/desktop/private/processor/interaction/InteractionContext.cpp",
            "plugins/desktop/private/processor/interaction/InteractionContext.hpp",
        ):
            with self.subTest(path=relative):
                text = (ROOT / relative).read_text()
                self.assertNotRegex(text, r"\b(?:SpeechProcessor|SpeechPlayer|MiniMaxSpeechSource|AudioPlayer)\b|姐姐，你好喵")

    def test_core_does_not_import_domain_contracts(self):
        for directory in ("sdk/event", "sdk/plugin"):
            for path in (ROOT / directory).glob("*"):
                if path.suffix not in (".hpp", ".cpp"):
                    continue
                with self.subTest(path=path):
                    self.assertNotRegex(path.read_text(), r'#include\s+"(?:contracts|processor/(?!Processor.hpp))/' )

    def test_application_does_not_link_speech(self):
        for relative in ("platform/CMakeLists.txt", "plugins/desktop/CMakeLists.txt"):
            text = (ROOT / relative).read_text()
            links = re.findall(r"target_link_libraries\([^)]*\)", text, re.S)
            for link in links:
                self.assertNotIn("deskwaifu_speech", link)

    def test_control_shell_has_no_page_business(self):
        text = (ROOT / "platform/control/MainWindow.cpp").read_text()
        for command in ("get_status", "get_speech_settings", "get_hotkeys",
                        "get_transform_settings", "get_render_device",
                        "set_mouse_look", "set_hover_opacity"):
            with self.subTest(command=command):
                self.assertNotIn('"' + command + '"', text)
        for widget in ("speechSettingsGroup", "hotkeyGroup", "modelGroup",
                       "renderDeviceGroup", "responseLog"):
            self.assertNotIn(widget, (ROOT / "platform/control/MainWindow.ui").read_text())

    def test_gui_pages_do_not_reach_into_shell(self):
        paths = list((ROOT / "plugins").glob("*/gui/*"))
        self.assertTrue(paths, "GUI code must live under its owning plugin")
        for path in paths:
            if path.suffix in (".hpp", ".cpp"):
                with self.subTest(path=path):
                    self.assertNotRegex(path.read_text(), r'#include\s+"(?:(?:ui_)?MainWindow|ControlHostServices)')
                    if path.parent.parent.name != "ipc":
                        self.assertNotRegex(path.read_text(), r'#include\s+"DeskWaifuClient')

    def test_control_does_not_assemble_domain_plugins(self):
        text = (ROOT / "platform/control/CMakeLists.txt").read_text()
        self.assertNotIn("target_sources(speech_plugin", text)
        self.assertNotIn("Tab.cpp", text)
        for domain in ("system", "interaction", "model", "speech"):
            self.assertTrue((ROOT / "plugins" / domain / "gui").is_dir())

    def test_each_plugin_owns_its_build_definition(self):
        plugins = ROOT / "plugins"
        parent = (plugins / "CMakeLists.txt").read_text()
        self.assertNotIn(".cpp", parent)
        self.assertNotIn("target_sources(", parent)
        for name in ("desktop", "perception", "ipc", "reading_clip", "speech", "system", "interaction", "model"):
            with self.subTest(plugin=name):
                self.assertIn(f"add_subdirectory({name})", parent)
                self.assertIn(f"deskwaifu_add_plugin({name}",
                              (plugins / name / "CMakeLists.txt").read_text())


if __name__ == "__main__":
    unittest.main()
