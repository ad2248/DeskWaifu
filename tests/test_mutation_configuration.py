import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mutations", ROOT / "scripts/check-test-mutations.py")
mutations = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mutations)


class MutationConfigurationTest(unittest.TestCase):
    def fixture(self, root, body):
        source, build = root / "source", root / "parent"
        source.mkdir()
        build.mkdir()
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(mutation_fixture NONE)\n' + body)
        (build / "CMakeCache.txt").write_text(
            "SDK_ROOT_PATH:PATH=/unused/sdk\nGLEW_PATH:PATH=/unused/glew\n"
            "DESKWAIFU_BUILD_OCR:BOOL=OFF\n")
        return source, build

    def invoke(self, source, build, extra=()):
        with patch.object(mutations, "ROOT", source), patch.object(mutations, "MUTATIONS", []), \
                patch.object(sys, "argv", ["check-test-mutations.py", str(build), *extra]):
            return mutations.main()

    def test_child_configure_does_not_enable_unused_native_ocr(self):
        with tempfile.TemporaryDirectory(prefix="mutation-config-") as directory:
            source, build = self.fixture(Path(directory),
                'option(DESKWAIFU_BUILD_OCR "Native OCR" ON)\n'
                'if(DESKWAIFU_BUILD_OCR)\n'
                '  message(FATAL_ERROR "Unexpected native OCR dependency in mutation build")\n'
                'endif()\n')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(self.invoke(source, build), 0)
            logs = list((build / "test-logs/mutations").glob("run-*/configure.log"))
            self.assertEqual(len(logs), 1)
            self.assertIn("Configuring done", logs[0].read_text())

    def test_configure_failure_surfaces_cmake_reason_and_retains_log(self):
        with tempfile.TemporaryDirectory(prefix="mutation-config-") as directory:
            source, build = self.fixture(Path(directory), 'message(FATAL_ERROR "fixture diagnostic sentinel")\n')
            with self.assertRaises(SystemExit) as error:
                self.invoke(source, build)
            self.assertIn("fixture diagnostic sentinel", str(error.exception))
            logs = list((build / "test-logs/mutations").glob("run-*/configure.log"))
            self.assertEqual(len(logs), 1)
            self.assertIn("fixture diagnostic sentinel", logs[0].read_text())

    def test_unknown_base_refuses_instead_of_reporting_a_partial_guess(self):
        with tempfile.TemporaryDirectory(prefix="mutation-config-") as directory:
            source, build = self.fixture(Path(directory), "")
            with self.assertRaises(SystemExit) as error:
                self.invoke(source, build, ["--base", "no-such-ref"])
            self.assertIn("cannot select mutations for no-such-ref", str(error.exception))


class ChangedPathsTest(unittest.TestCase):
    """The baseline must be the fork point, not the base branch tip."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mutation-diff-")
        self.repo = Path(self.directory.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "test")
        self.write("README.md", "root\n")
        self.commit("root")

    def tearDown(self):
        self.directory.cleanup()

    def git(self, *arguments):
        result = subprocess.run(["git", "-C", str(self.repo), *arguments],
                                capture_output=True, text=True, check=True)
        return result.stdout

    def write(self, relative, text):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-qm", message)

    def branch(self, name, create=False):
        self.git("checkout", "-q", *(["-B"] if create else []), name)

    def diff(self, base):
        # 分支推进基线后要回到 PR 分支，否则 HEAD 就是基线自己
        self.branch("feature")
        with patch.object(mutations, "ROOT", self.repo):
            return mutations.changed_paths(base)

    def test_base_branch_advancing_does_not_add_its_paths_to_this_branch(self):
        self.branch("feature", create=True)
        self.write("plugins/speech/private/processor/speech/SpeechPlayer.cpp", "pr\n")
        self.commit("pr work")
        self.branch("main")
        self.write("platform/README-test.md", "landed after the fork\n")
        self.commit("base advances")

        changed = self.diff("main")
        self.assertEqual(changed, ["plugins/speech/private/processor/speech/SpeechPlayer.cpp"])
        self.assertEqual([m["mutation"] for m in mutations.select(changed)], ["late-speech"])

    def test_base_branch_advancing_does_not_select_a_mutation_for_its_own_test(self):
        self.branch("feature", create=True)
        self.write("docs/需求描述/测试与门禁.md", "pr\n")
        self.commit("pr work")
        self.branch("main")
        self.write("plugins/desktop/tests/ModelPointerTest.cpp", "landed after the fork\n")
        self.commit("base advances")

        changed = self.diff("main")
        self.assertEqual(changed, ["docs/需求描述/测试与门禁.md"])
        self.assertEqual(mutations.select(changed), [])

    def test_commits_already_on_the_base_branch_are_not_this_branch_s_changes(self):
        self.write("cmake/DeskWaifuTest.cmake", "on base\n")
        self.commit("earlier base work")
        self.branch("feature", create=True)
        self.write("plugins/desktop/private/app/LAppDelegate.cpp", "pr\n")
        self.commit("pr work")

        changed = self.diff("main")
        self.assertEqual(changed, ["plugins/desktop/private/app/LAppDelegate.cpp"])
        self.assertEqual([m["mutation"] for m in mutations.select(changed)], ["cursor-assembly"])

    def test_shallow_history_is_refused_rather_than_trusted(self):
        self.branch("feature", create=True)
        self.write("plugins/desktop/private/app/LAppDelegate.cpp", "pr\n")
        self.commit("pr work")
        shallow = self.repo / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth=1", "--branch", "feature",
                        f"file://{self.repo}", str(shallow)], check=True)

        with patch.object(mutations, "ROOT", shallow), self.assertRaises(mutations.UnprovableDiff) as error:
            mutations.changed_paths("origin/main")
        self.assertIn("shallow", str(error.exception))


class MutationSelectionTest(unittest.TestCase):
    def select(self, *changed):
        return [mutation["mutation"] for mutation in mutations.select(list(changed))]

    def test_owned_production_path_selects_its_mutation(self):
        self.assertEqual(self.select("plugins/speech/private/processor/speech/SpeechPlayer.cpp"),
                         ["late-speech"])

    def test_test_source_change_selects_the_mutation_it_must_catch(self):
        self.assertEqual(self.select("plugins/desktop/tests/ModelPointerTest.cpp"),
                         ["cursor-timer", "cursor-assembly"])

    def test_shared_target_selection_cannot_prove_a_mapping_and_runs_everything(self):
        for path in ("platform/plugin/PluginManager.cpp", "cmake/DeskWaifuTest.cmake",
                     "sdk/include/EventLoop.hpp", "CMakeLists.txt", "thirdParty/glew/src/glew.c"):
            with self.subTest(path=path):
                self.assertEqual(len(self.select(path)), len(mutations.MUTATIONS))

    def test_unmapped_change_runs_nothing_and_says_so(self):
        self.assertEqual(self.select("README.md", "docs/需求描述/发布打包.md"),
                         [])
        self.assertEqual(self.select(".gitea/workflows/ci.yml"), [])

    def test_unmapped_build_path_cannot_be_proven_safe_and_runs_everything(self):
        for path in ("plugins/desktop/tests/CMakeLists.txt", "plugins/speech/CMakeLists.txt",
                     "scripts/build.sh"):
            with self.subTest(path=path):
                self.assertEqual(len(self.select(path)), len(mutations.MUTATIONS))

    def test_mapped_and_inert_paths_together_select_only_the_mapped_mutation(self):
        self.assertEqual(self.select("plugins/speech/private/processor/speech/SpeechPlayer.cpp",
                                     "README.md"),
                         ["late-speech"])

    def test_clean_tree_runs_the_full_set(self):
        self.assertEqual(len(mutations.select(None)), len(mutations.MUTATIONS))
        self.assertEqual(len(mutations.select([])), len(mutations.MUTATIONS))

    def test_empty_selection_reports_partial_scope_and_passes(self):
        with tempfile.TemporaryDirectory(prefix="mutation-select-") as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text(
                "SDK_ROOT_PATH:PATH=/unused/sdk\nGLEW_PATH:PATH=/unused/glew\n")
            with patch.object(mutations, "ROOT", build), \
                    patch.object(sys, "argv", ["check-test-mutations.py", str(build)]), \
                    patch.object(mutations, "changed_paths", lambda base: ["README.md"]):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(mutations.main(), 0)
            self.assertIn('"selected": 0', output.getvalue())


if __name__ == "__main__":
    unittest.main()
