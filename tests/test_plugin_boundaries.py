"""Guard the migrated boundary, not the yet-to-be-migrated legacy application."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PluginBoundaries(unittest.TestCase):
    def test_host_has_no_speech_adapter(self):
        for relative in (
            "src/main.cpp", "src/app/LAppDelegate.cpp", "src/app/LAppDelegate.hpp",
            "src/processor/ProcessorRuntime.hpp",
            "src/processor/command/CommandProcessor.hpp",
            "src/processor/interaction/InteractionProcessor.cpp",
            "src/processor/interaction/InteractionProcessor.hpp",
            "src/processor/interaction/InteractionContext.cpp",
            "src/processor/interaction/InteractionContext.hpp",
        ):
            with self.subTest(path=relative):
                text = (ROOT / relative).read_text()
                self.assertNotRegex(text, r"(?i)speech|minimax|姐姐，你好喵")

    def test_core_does_not_import_domain_contracts(self):
        for directory in ("src/event", "src/plugin"):
            for path in (ROOT / directory).glob("*"):
                if path.suffix not in (".hpp", ".cpp"):
                    continue
                with self.subTest(path=path):
                    self.assertNotRegex(path.read_text(), r'#include\s+"(?:contracts|processor/(?!Processor.hpp))/' )

    def test_application_does_not_link_speech(self):
        for relative in ("src/CMakeLists.txt", "src/processor/CMakeLists.txt"):
            text = (ROOT / relative).read_text()
            links = re.findall(r"target_link_libraries\([^)]*\)", text, re.S)
            for link in links:
                self.assertNotIn("deskwaifu_speech", link)

    def test_control_shell_has_no_page_business(self):
        text = (ROOT / "control/MainWindow.cpp").read_text()
        for command in ("get_status", "get_speech_settings", "get_hotkeys",
                        "get_transform_settings", "get_render_device",
                        "set_mouse_look", "set_hover_opacity"):
            with self.subTest(command=command):
                self.assertNotIn('"' + command + '"', text)
        for widget in ("speechSettingsGroup", "hotkeyGroup", "modelGroup",
                       "renderDeviceGroup", "responseLog"):
            self.assertNotIn(widget, (ROOT / "control/MainWindow.ui").read_text())

    def test_gui_pages_do_not_reach_into_shell(self):
        for path in (ROOT / "control/tabs").glob("*"):
            if path.suffix in (".hpp", ".cpp"):
                with self.subTest(path=path):
                    self.assertNotRegex(path.read_text(), r'#include\s+"(?:ui_)?MainWindow')


if __name__ == "__main__":
    unittest.main()
