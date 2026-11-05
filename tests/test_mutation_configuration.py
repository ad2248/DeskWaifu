import contextlib
import importlib.util
import io
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

    def test_test_helper_header_selects_every_mutation_whose_target_compiles_it(self):
        for path, expected in (
            ("plugins/desktop/tests/ModelProcessorTestHelpers.hpp",
             ["gpu-release", "cursor-timer", "cursor-assembly", "gpu-context-guard"]),
            ("plugins/desktop/tests/TestApplication.hpp", ["cursor-timer", "cursor-assembly"]),
            ("plugins/desktop/tests/DispatchedInput.hpp", ["cursor-timer", "cursor-assembly"]),
            ("plugins/desktop/tests/EventLoopFence.hpp", ["hover"]),
            ("plugins/speech/tests/EventLoopFence.hpp", ["late-speech"]),
        ):
            with self.subTest(path=path):
                self.assertEqual(self.select(path), expected)

    def test_support_paths_exist_so_a_rename_cannot_leave_a_stale_mapping(self):
        for mutation in mutations.MUTATIONS:
            for path in mutation.get("support", ()):
                with self.subTest(mutation=mutation["mutation"], path=path):
                    self.assertTrue((ROOT / path).is_file(), f"{path} no longer exists")

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
