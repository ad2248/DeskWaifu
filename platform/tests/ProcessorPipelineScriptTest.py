"""Exercise the standalone pipeline script with the configured CMake version."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("CheckProcessorPipeline.cmake")


class ProcessorPipelineScriptTest(unittest.TestCase):
    def test_adapter_arguments_with_and_without_speech(self):
        for speech in (False, True):
            with self.subTest(speech=speech), tempfile.TemporaryDirectory(prefix="dw pipeline ") as directory:
                root = Path(directory)
                log = root / "calls.jsonl"
                programs = {name: root / name for name in ("READING", "PERCEPTION", "IPC", "DYNAMIC", "SPEECH")}
                for program in programs.values():
                    program.write_text(
                        f"#!{sys.executable}\n"
                        "import json, sys\n"
                        f"with open({str(log)!r}, 'a') as stream:\n"
                        "    stream.write(json.dumps([sys.argv[0], sys.argv[1:]]) + '\\n')\n"
                    )
                    program.chmod(0o700)
                paths = {name: str(root / name) for name in ("OCR_HELPER", "READING_PLUGIN", "PERCEPTION_PLUGIN")}
                definitions = {name: str(program) for name, program in programs.items() if name != "SPEECH"}
                definitions.update(paths)
                environment = os.environ.copy()
                environment.pop("DESKWAIFU_PIPELINE_SPEECH", None)
                if speech:
                    environment["DESKWAIFU_PIPELINE_SPEECH"] = str(programs["SPEECH"])
                result = subprocess.run(
                    [CMAKE, *(f"-D{name}={value}" for name, value in definitions.items()), "-P", str(SCRIPT)],
                    env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stdout)
                expected = [
                    [str(programs["READING"]), []],
                    [str(programs["PERCEPTION"]), [paths["OCR_HELPER"]]],
                    [str(programs["IPC"]), []],
                ]
                if speech:
                    expected.append([str(programs["SPEECH"]), []])
                expected.append([str(programs["DYNAMIC"]), [paths[name] for name in
                                 ("READING_PLUGIN", "PERCEPTION_PLUGIN", "OCR_HELPER")]])
                self.assertEqual([json.loads(line) for line in log.read_text().splitlines()], expected, result.stdout)


if __name__ == "__main__":
    CMAKE = sys.argv.pop(1)
    unittest.main()
