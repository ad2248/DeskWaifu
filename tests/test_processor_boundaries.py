"""Static ownership guardrails for known bypasses; not whole-program callback analysis."""
from pathlib import Path
import re
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "src"
PROCESSORS = {
    "ModelProcessor": "model", "InteractionProcessor": "interaction",
    "InputListener": "input", "IpcProcessor": "ipc", "CommandProcessor": "command",
    "OcrProcessor": "ocr", "ReadingClipProcessor": "reading_clip",
    "SpeechProcessor": "speech", "SignalProcessor": "signal",
    "ScreenshotProcessor": "screenshot", "ImageProcessor": "image",
}


def violations(path, text):
    parts = path.parts
    if len(parts) < 3 or parts[0] != "processor" or "tests" in parts:
        return []
    owner = parts[1]
    errors = []
    for include in re.findall(r'#include\s+"([^"]+)"', text):
        resolved = (SOURCE / path.parent / include).resolve()
        if not resolved.exists():
            resolved = (SOURCE / include).resolve()
        try:
            target = resolved.relative_to(SOURCE).parts
        except ValueError:
            continue
        if target[0] == "app" or (len(target) > 2 and target[0] == "processor" and target[1] != owner):
            errors.append(f"foreign implementation include: {include}")
    code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', '', text, flags=re.S)
    for name, domain in PROCESSORS.items():
        if domain != owner and re.search(r'\b' + name + r'\b', code):
            errors.append(f"concrete peer type: {name}")
    if re.search(r'\bLAppDelegate\b|std::function\s*<\s*ModelMetadata|SetModelChanged', code):
        errors.append("application/callback business bypass")
    return errors


class ProcessorBoundaries(unittest.TestCase):
    def test_ci_variant_paths_exist(self):
        workflow = (SOURCE.parent / ".gitea/workflows/ci.yml").read_text()
        paths = re.findall(r"endswith\('(/src/[^']+)'\)", workflow)
        self.assertTrue(paths, "CI must verify the variant's manager compilation")
        for path in paths:
            self.assertTrue((SOURCE.parent / path.lstrip("/")).is_file(), path)
        self.assertIn("expected in shlex.split(entry['command'])", workflow)

    def test_production_tree(self):
        errors = []
        for path in SOURCE.rglob("*"):
            if path.suffix in (".hpp", ".cpp"):
                errors += [f"{path.relative_to(SOURCE)}: {e}" for e in violations(path.relative_to(SOURCE), path.read_text())]
        self.assertEqual(errors, [])
        self.assertEqual(sorted(p.name for p in SOURCE.iterdir() if p.is_file()), ["CMakeLists.txt", "main.cpp"])
        for path in (SOURCE / "contracts").glob("*.hpp"):
            self.assertNotRegex(path.read_text(), r'#include\s+"(?:processor|app)/')
        delegate = (SOURCE / "app/LAppDelegate.hpp").read_text()
        self.assertNotRegex(delegate, r'\b(?:Models|Processors|Ocr|Service|Settings|SelectOutput|MoveScreenDrag)\s*\(')
        runtime = (SOURCE / "processor/ProcessorRuntime.hpp").read_text()
        self.assertNotRegex(runtime, r'\b(?:Client|Reading|Ocr|SpeakGreeting|ReadClipboard)\s*\(')
        command = (SOURCE / "processor/command/CommandProcessor.hpp").read_text()
        self.assertNotIn("ICommandService", command)
        self.assertNotIn("std::function", command)
        ipc = (SOURCE / "processor/ipc/IpcProcessor.cpp").read_text()
        self.assertIn("_ipc.SetCommandService(*this)", ipc)

    def test_negative_fixtures(self):
        path = Path("processor/command/Bad.hpp")
        for code in ('#include "processor/model/ModelProcessor.hpp"',
                     '#include "../model/ModelProcessor.hpp"',
                     'class ModelProcessor; ModelProcessor* model;',
                     'class LAppDelegate; LAppDelegate& app;',
                     'std::function<ModelMetadata()> metadata;'):
            with self.subTest(code=code):
                self.assertTrue(violations(path, code))
        self.assertFalse(violations(path, '#include "contracts/RenderFrame.hpp"\nIProcessor* model;'))


if __name__ == "__main__":
    unittest.main()
