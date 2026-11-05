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

    def test_mutation_step_scopes_the_selection_to_the_pull_request_merge_base(self):
        """锁定选择基线：去掉 --base 或改用 merge-base 会静默退化为全集或空集。"""
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        text = workflow.read_text()
        step = re.search(
            r"- name: Verify behavioral mutation interception\n(?:.*\n)*?(?=\n {6}- name: )", text)
        self.assertIsNotNone(step, "the mutation step must stay a single named step")
        body = step.group(0)

        self.assertIn('[[ "${{ github.event_name }}" == "pull_request" ]]', body,
                      "Only pull requests have a base branch to diff against")
        self.assertIn('merge_base="$(git merge-base "$base_ref" HEAD)"', body,
                      "The baseline must be the merge base, not the base branch tip")
        self.assertIn('args+=(--base "$merge_base")', body,
                      "The mutation script needs --base, or it silently runs everything")
        # depth 抓取会把完整克隆重新变成浅克隆，脚本随即拒绝选择。
        self.assertNotIn("--depth", body)

    def test_mutation_step_fails_loudly_when_the_baseline_fetch_yields_nothing(self):
        """空的 --base 会让脚本把「取不到基线」当成「影响全集」，必须显式失败。"""
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        step = re.search(r"- name: Verify behavioral mutation interception\n(?:.*\n)*?(?=\n {6}- name: )",
                         workflow.read_text())
        self.assertIsNotNone(step)
        body = step.group(0)

        self.assertIn('git fetch --no-tags origin', body,
                      "Without a fetch the baseline ref does not exist on a fresh clone")
        fetched = re.search(r"git fetch[^\n]*\n((?:[ \t]+.*\n)*)", body).group(1)
        self.assertIn('git rev-parse --verify --quiet "$base_ref^{commit}"', body,
                      "A fetch that produced nothing must fail before selecting")
        self.assertIn("exit 1", fetched, "The empty-fetch check must abort the step")
        self.assertIn('test -n "$merge_base"', body,
                      "An empty merge base must not reach the script as --base ''")

    def test_mutation_checkout_keeps_full_history(self):
        workflow = pathlib.Path(__file__).resolve().parents[1] / "workflows/ci.yml"
        text = workflow.read_text()
        self.assertRegex(text, r"fetch-depth: 0")

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
