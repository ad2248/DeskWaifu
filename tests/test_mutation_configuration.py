import contextlib
import importlib.util
import io
import json
from pathlib import Path
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
            self.assertIn("cannot diff against no-such-ref", str(error.exception))

    def test_changed_paths_keeps_non_ascii_paths_literal(self):
        import subprocess
        with tempfile.TemporaryDirectory(prefix="mutation-diff-") as directory:
            repo = Path(directory)
            def git(*args):
                return subprocess.run(["git", "-C", str(repo), *args], check=True,
                                      capture_output=True, text=True).stdout
            git("init", "--initial-branch=main")
            git("config", "user.name", "Test")
            git("config", "user.email", "test@example.invalid")
            (repo / "docs").mkdir()
            (repo / "docs/需求描述").mkdir()
            (repo / "docs/需求描述/发布打包.md").write_text("base\n")
            git("add", ".")
            git("commit", "-m", "base")
            base = git("rev-parse", "HEAD").strip()
            (repo / "docs/需求描述/发布打包.md").write_text("changed\n")
            git("commit", "-am", "change")
            with patch.object(mutations, "ROOT", repo):
                paths = mutations.changed_paths(base)
            self.assertEqual(paths, ["docs/需求描述/发布打包.md"],
                             "Quoted paths would defeat prefix matching and widen the selection")
            self.assertEqual(mutations.select(paths), [])


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
            summary = build / "step-summary.md"
            with patch.object(mutations, "ROOT", build), \
                    patch.object(sys, "argv", ["check-test-mutations.py", str(build)]), \
                    patch.dict("os.environ", {"GITHUB_STEP_SUMMARY": str(summary)}), \
                    patch.object(mutations, "changed_paths", lambda base: ["README.md"]):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(mutations.main(), 0)
            self.assertIn("已选 0 项", summary.read_text(),
                          "通过时不上传产物，日志摘要要说明为何没跑")
            self.assertIn('"selected": 0', output.getvalue())
            record = json.loads((build / "test-logs/mutations/selection.json").read_text())
            self.assertFalse(record["full"])
            self.assertEqual(record["selected"], [])
            self.assertEqual(record["changed"], ["README.md"])
            self.assertEqual(record["reason"], "no changed path affects a mutation")
            self.assertEqual(list((build / "test-logs/mutations").glob("run-*")), [],
                             "跳过的运行不留下 run 目录")

    def test_selection_is_written_even_when_the_run_never_builds(self):
        def failing(command, log):
            log.write_text("configure failed\n")
            return 1

        with tempfile.TemporaryDirectory(prefix="mutation-select-") as directory:
            source, build = Path(directory) / "source", Path(directory) / "parent"
            source.mkdir()
            build.mkdir()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\nproject(mutation_fixture NONE)\n")
            (build / "CMakeCache.txt").write_text(
                "SDK_ROOT_PATH:PATH=/unused/sdk\nGLEW_PATH:PATH=/unused/glew\n")
            with patch.object(mutations, "ROOT", source), \
                    patch.object(mutations, "MUTATIONS", []), \
                    patch.object(mutations, "run", failing), \
                    patch.object(sys, "argv", ["check-test-mutations.py", str(build)]):
                with contextlib.redirect_stdout(io.StringIO()), \
                        self.assertRaises(SystemExit):
                    mutations.main()
            record = json.loads((build / "test-logs/mutations/selection.json").read_text())
            self.assertTrue(record["full"])
            self.assertIsNone(record["base"])
            self.assertEqual(record["reason"], "no base commit; only the full set is provable")


if __name__ == "__main__":
    unittest.main()
