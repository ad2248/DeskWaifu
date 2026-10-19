import pathlib
import re
import shlex
import unittest


class CiConcurrencyTest(unittest.TestCase):
    def test_all_builds_use_sixteen_compiler_jobs(self):
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        builds = re.findall(r"^\s*(?:run:\s*)?(cmake --build [^\n]+)", workflow.read_text(), re.MULTILINE)
        self.assertEqual(len(builds), 2, "Check both the main build and render-target variants")
        for build in builds:
            with self.subTest(build=build):
                arguments = shlex.split(build)
                self.assertIn("--parallel", arguments)
                self.assertEqual(arguments[arguments.index("--parallel") + 1], "16")


if __name__ == "__main__":
    unittest.main()
