"""Independent source-level ownership checks, including hostile include spellings."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
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
    if parts[:1] == ("plugins",) and len(parts) > 1:
        return parts[1]
    return parts[0] if parts else None


def violations(path, text):
    if "tests" in path.parts or "testing" in path.parts:
        return []
    current = owner(path)
    errors = []
    for include in re.findall(r'^\s*#\s*include\s*["<]([^">]+)[">]', text, re.M):
        roots = [ROOT / path.parent, ROOT / "sdk", ROOT]
        if path.parts[:1] == ("plugins",):
            roots += [ROOT / "plugins" / current / suffix for suffix in ("private", "gui", "")]
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
            if target == "platform" or relative.parts[:1] == ("plugins",):
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
    def test_production_tree(self):
        errors = []
        for tree in (ROOT / "sdk", ROOT / "platform", ROOT / "plugins"):
            for path in tree.rglob("*"):
                if path.suffix in (".hpp", ".cpp"):
                    relative = path.relative_to(ROOT)
                    errors += [f"{relative}: {error}" for error in violations(relative, path.read_text())]
        self.assertEqual(errors, [])
        for retired in ("src", "control"):
            self.assertFalse(any(path.suffix in (".cpp", ".hpp") or path.name == "CMakeLists.txt"
                                 for path in (ROOT / retired).rglob("*")), retired)
        for path in (ROOT / "sdk/contracts").glob("*.hpp"):
            self.assertNotRegex(path.read_text(), r'#include\s+"(?:processor/(?!Processor.hpp)|app)/')
        delegate = (ROOT / "plugins/desktop/private/app/LAppDelegate.hpp").read_text()
        self.assertNotRegex(delegate, r'\b(?:Models|Processors|Ocr|Service|Settings|SelectOutput|MoveScreenDrag)\s*\(')
        command = (ROOT / "plugins/ipc/private/processor/command/CommandProcessor.hpp").read_text()
        self.assertNotIn("ICommandService", command)
        self.assertNotIn("std::function", command)
        ipc = (ROOT / "plugins/ipc/private/processor/ipc/IpcProcessor.cpp").read_text()
        self.assertIn("_ipc.SetCommandService(*this)", ipc)

if __name__ == "__main__":
    unittest.main()
