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


if __name__ == "__main__":
    unittest.main()
