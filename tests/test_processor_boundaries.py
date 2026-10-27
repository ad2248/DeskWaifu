"""Independent source-level ownership checks, including hostile include spellings."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src"
PROCESSORS = {
    "ModelProcessor": "desktop", "InteractionProcessor": "desktop",
    "InputListener": "desktop", "SignalProcessor": "desktop",
    "IpcProcessor": "ipc", "CommandProcessor": "ipc",
    "OcrProcessor": "perception", "ImageProcessor": "perception",
    "ScreenshotProcessor": "perception", "ReadingClipProcessor": "reading_clip",
    "SpeechProcessor": "speech",
}


def owner(path):
    parts = path.parts
    if parts[:2] == ("src", "plugins") and len(parts) > 2:
        return parts[2]
    return parts[0] if parts else None


def violations(path, text):
    if "tests" in path.parts or "testing" in path.parts:
        return []
    current = owner(path)
    errors = []
    for include in re.findall(r'^\s*#\s*include\s*["<]([^">]+)[">]', text, re.M):
        roots = [ROOT / path.parent, ROOT / "sdk", ROOT]
        if path.parts[:2] == ("src", "plugins"):
            roots += [ROOT / "src/plugins" / current / suffix for suffix in ("private", "gui", "")]
        candidates = [Path(include)] if Path(include).is_absolute() else [base / include for base in roots]
        for candidate in candidates:
            if not candidate.is_file():
                continue
            try:
                relative = candidate.resolve().relative_to(ROOT)
            except ValueError:
                break
            target = owner(relative)
            if target in ("sdk", current):
                break
            if target == "platform" or relative.parts[:2] == ("src", "plugins"):
                errors.append(f"foreign implementation include: {include}")
            break
    code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', '', text, flags=re.S)
    for name, domain in PROCESSORS.items():
        if domain != current and re.search(r'\b' + name + r'\b', code):
            errors.append(f"concrete peer type: {name}")
    if current != "desktop" and re.search(r'\bLAppDelegate\b|std::function\s*<\s*ModelMetadata|SetModelChanged', code):
        errors.append("application/callback business bypass")
    return errors


class ProcessorBoundaries(unittest.TestCase):
    def test_ci_variant_paths_exist(self):
        workflow = (ROOT / ".gitea/workflows/ci.yml").read_text()
        paths = re.findall(r"endswith\('(/(?:src|sdk|platform)/[^']+)'\)", workflow)
        self.assertTrue(paths, "CI must verify the variant's manager compilation")
        for path in paths:
            self.assertTrue((ROOT / path.lstrip("/")).is_file(), path)
        self.assertIn("expected in shlex.split(entry['command'])", workflow)

    def test_production_tree(self):
        errors = []
        for tree in (ROOT / "sdk", ROOT / "platform", ROOT / "src/plugins"):
            for path in tree.rglob("*"):
                if path.suffix in (".hpp", ".cpp"):
                    relative = path.relative_to(ROOT)
                    errors += [f"{relative}: {error}" for error in violations(relative, path.read_text())]
        self.assertEqual(errors, [])
        for retired in ("src/app", "src/render", "src/processor", "src/contracts", "src/event", "src/plugin", "control"):
            self.assertFalse(any(path.suffix in (".cpp", ".hpp") or path.name == "CMakeLists.txt"
                                 for path in (ROOT / retired).rglob("*")), retired)
        for path in (ROOT / "sdk/contracts").glob("*.hpp"):
            self.assertNotRegex(path.read_text(), r'#include\s+"(?:processor/(?!Processor.hpp)|app)/')
        delegate = (ROOT / "src/plugins/desktop/private/app/LAppDelegate.hpp").read_text()
        self.assertNotRegex(delegate, r'\b(?:Models|Processors|Ocr|Service|Settings|SelectOutput|MoveScreenDrag)\s*\(')
        command = (ROOT / "src/plugins/ipc/private/processor/command/CommandProcessor.hpp").read_text()
        self.assertNotIn("ICommandService", command)
        self.assertNotIn("std::function", command)
        ipc = (ROOT / "src/plugins/ipc/private/processor/ipc/IpcProcessor.cpp").read_text()
        self.assertIn("_ipc.SetCommandService(*this)", ipc)

    def test_negative_fixtures(self):
        path = Path("src/plugins/ipc/private/processor/command/Bad.hpp")
        foreign = ROOT / "src/plugins/desktop/private/processor/model/ModelProcessor.hpp"
        for code in (
            f'#include "{foreign}"',
            '#include "../../../../desktop/private/processor/model/ModelProcessor.hpp"',
            '#include <src/plugins/desktop/private/processor/model/ModelProcessor.hpp>',
            'class ModelProcessor; ModelProcessor* model;',
            'class LAppDelegate; LAppDelegate& app;',
            'std::function<ModelMetadata()> metadata;',
        ):
            with self.subTest(code=code):
                self.assertTrue(violations(path, code))
        self.assertFalse(violations(path, '#include "contracts/RenderFrame.hpp"\nIProcessorPlugin* model;'))
        self.assertFalse(violations(Path("src/plugins/desktop/private/app/Runtime.hpp"),
                                    '#include "processor/model/ModelProcessor.hpp"\nModelProcessor* model;'))


if __name__ == "__main__":
    unittest.main()
