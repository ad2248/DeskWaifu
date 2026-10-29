"""Ownership rules for the SDK, platform and dynamically loaded domains."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PluginBoundaries(unittest.TestCase):
    def test_host_has_no_speech_adapter(self):
        for relative in (
            "platform/main.cpp", "src/plugins/desktop/private/app/LAppDelegate.cpp",
            "src/plugins/desktop/private/app/LAppDelegate.hpp",
            "src/plugins/ipc/private/processor/command/CommandProcessor.hpp",
            "src/plugins/desktop/private/processor/interaction/InteractionProcessor.cpp",
            "src/plugins/desktop/private/processor/interaction/InteractionProcessor.hpp",
            "src/plugins/desktop/private/processor/interaction/InteractionContext.cpp",
            "src/plugins/desktop/private/processor/interaction/InteractionContext.hpp",
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
        for relative in ("platform/CMakeLists.txt", "src/plugins/desktop/CMakeLists.txt"):
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
        paths = list((ROOT / "src/plugins").glob("*/gui/*"))
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
        for domain in ("system", "interaction", "model", "log", "speech"):
            self.assertTrue((ROOT / "src/plugins" / domain / "gui").is_dir())

    def test_each_plugin_owns_its_build_definition(self):
        plugins = ROOT / "src/plugins"
        parent = (plugins / "CMakeLists.txt").read_text()
        self.assertNotIn(".cpp", parent)
        self.assertNotIn("target_sources(", parent)
        for name in ("desktop", "perception", "ipc", "reading_clip", "speech", "system", "interaction", "model", "log"):
            with self.subTest(plugin=name):
                self.assertIn(f"add_subdirectory({name})", parent)
                self.assertIn(f"deskwaifu_add_plugin({name}",
                              (plugins / name / "CMakeLists.txt").read_text())


if __name__ == "__main__":
    unittest.main()
