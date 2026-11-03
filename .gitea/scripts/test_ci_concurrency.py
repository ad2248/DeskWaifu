import pathlib
import re
import shlex
import unittest


class CiConcurrencyTest(unittest.TestCase):
    def test_package_builds_use_twenty_four_compiler_jobs(self):
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        self.assertIn("CMAKE_BUILD_PARALLEL_LEVEL: 24", workflow.read_text())

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

    def test_dispatch_exposes_clean_build_opt_out(self):
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        text = workflow.read_text()
        self.assertRegex(text, r"clean:\s*\n\s*description: '[^']*编译缓存'")
        self.assertIn("type: boolean", text)
        self.assertEqual(text.count("DESKWAIFU_CLEAN_BUILD: ${{ github.event.inputs.clean }}"), 2,
                         "Both CI jobs must honor the clean build option")

    def test_all_configures_use_the_persistent_compiler_cache(self):
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        text = workflow.read_text()
        self.assertEqual(text.count("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"), 3,
                         "Check the main build, render-target variants and sanitizer build")
        self.assertIn('echo "CCACHE_DIR=/cache/ccache"', text)
        self.assertIn('echo "CCACHE_DIR=/cache/ccache-makepkg"', text)
        self.assertIn("CCACHE_DISABLE=1", text)
        self.assertNotIn("pacman -", text, "CI must use the pinned image instead of mid-job installs")


if __name__ == "__main__":
    unittest.main()
