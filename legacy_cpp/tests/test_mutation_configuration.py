import contextlib
import importlib.util
import io
import os
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
    def test_support_paths_exist_so_a_rename_cannot_leave_a_stale_mapping(self):
        # support 既可能是单个头文件，也可能是整个目录前缀，两者都必须真实存在
        for mutation in mutations.MUTATIONS:
            for path in mutation.get("support", ()):
                with self.subTest(mutation=mutation["mutation"], path=path):
                    target = ROOT / path.rstrip("/")
                    self.assertTrue(target.exists(), f"{path} no longer exists")
                    if path.endswith("/"):
                        self.assertTrue(target.is_dir(), f"{path} should be a directory prefix")
                    else:
                        self.assertTrue(target.is_file(), f"{path} should be a single file")

    def test_selection_reaches_job_summary_through_the_isolation_launcher(self):
        """CI 经由 test-environment.py 调用，摘要落盘才算真的写进 job summary。"""
        with tempfile.TemporaryDirectory(prefix="mutation-summary-") as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text(
                "SDK_ROOT_PATH:PATH=/unused/sdk\nGLEW_PATH:PATH=/unused/glew\n")
            summary = build / "step-summary.md"
            script = (
                "import importlib.util, sys\n"
                f"spec = importlib.util.spec_from_file_location('mutations', {str(ROOT / 'scripts/check-test-mutations.py')!r})\n"
                "mutations = importlib.util.module_from_spec(spec)\n"
                "spec.loader.exec_module(mutations)\n"
                "mutations.changed_paths = lambda base: ['README.md']\n"
                f"sys.argv = ['check-test-mutations.py', {str(build)!r}]\n"
                "sys.exit(mutations.main())\n")
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/test-environment.py"), sys.executable, "-c", script],
                env=dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)),
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            text = summary.read_text()
            self.assertIn("已选 0 项", text, "白名单不转发 GITHUB_STEP_SUMMARY 时摘要丢失")
            self.assertIn("改动 ['README.md']", text)

if __name__ == "__main__":
    unittest.main()
