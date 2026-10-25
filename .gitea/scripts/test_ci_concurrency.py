import pathlib
import re
import shlex
import unittest


class CiConcurrencyTest(unittest.TestCase):
    def test_all_builds_use_sixteen_compiler_jobs(self):
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        commands = workflow.read_text().replace("\\\n", " ")
        builds = re.findall(r"^\s*(?:run:\s*)?(cmake --build [^\n]+)", commands, re.MULTILINE)
        self.assertEqual(len(builds), 3, "Check the main build, render-target variants and sanitizer build")
        for build in builds:
            with self.subTest(build=build):
                arguments = shlex.split(build)
                self.assertIn("--parallel", arguments)
                self.assertEqual(arguments[arguments.index("--parallel") + 1], "16")


if __name__ == "__main__":
    unittest.main()
