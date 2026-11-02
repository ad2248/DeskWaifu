import pathlib
import re
import shlex
import unittest


class CiConcurrencyTest(unittest.TestCase):
    def test_all_builds_use_twenty_four_compiler_jobs(self):
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        commands = workflow.read_text().replace("\\\n", " ")
        builds = re.findall(r"^\s*(?:run:\s*)?(cmake --build [^\n]+)", commands, re.MULTILINE)
        self.assertEqual(len(builds), 3, "Check the main build, render-target variants and sanitizer build")
        for build in builds:
            with self.subTest(build=build):
                arguments = shlex.split(build)
                self.assertIn("--parallel", arguments)
                self.assertEqual(arguments[arguments.index("--parallel") + 1], "24")

    def test_mutation_builds_use_twenty_four_compiler_jobs(self):
        script = pathlib.Path(__file__).resolve().parents[2] / "scripts/check-test-mutations.py"
        parallelism = re.findall(r'"--parallel", "(\d+)"', script.read_text())
        self.assertEqual(parallelism, ["24", "24"], "Check both mutated and restored builds")


if __name__ == "__main__":
    unittest.main()
