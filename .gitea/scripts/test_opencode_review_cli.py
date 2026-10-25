"""Exercise the review invocation without contacting a model or posting reviews."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).with_name("opencode-review.sh")


class ReviewCliTest(unittest.TestCase):
    def test_v2_permissions_allow_tools_without_step_limit(self):
        source = SOURCE.read_text()
        config = json.loads(source.split("<<'JSON'\n", 1)[1].split("\nJSON", 1)[0])
        allow = {"action": "*", "resource": "*", "effect": "allow"}
        self.assertEqual(config["permissions"], [allow])
        self.assertEqual(config["agents"]["build"], {"permissions": [allow]})

    def test_review_prompt_allows_source_exploration_and_verification(self):
        source = SOURCE.read_text()
        prompt = source.split('\nprompt="$(cat <<EOF\n', 1)[1].split('\nEOF\n)"', 1)[0]
        self.assertIn("可自由使用工具", prompt)
        self.assertIn("按需执行构建或测试", prompt)
        self.assertIn("目标分支代码不是 PR head", prompt)
        sections = ("你是代码审查智能体", "审查注意事项：", "被审查的 PR 的上下文信息在：$workdir/context.md",
                    "被审查的 PR 的目标分支代码在：$repo_root", "被审查的 PR（diff 形式）在：$workdir/diff.patch",
                    "审查结果格式：", "## 结论", "## 阻塞问题", "## 非阻塞建议")
        positions = [prompt.index(section) for section in sections]
        self.assertEqual(positions, sorted(positions))
        for restriction in ("禁止调用 API", "仅根据附件", "不调用任何工具", "不要尝试其他命令"):
            self.assertNotIn(restriction, prompt)

    def test_review_uses_target_checkout_and_complete_pr_diff(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo = root / "target repository with spaces"
            repo.mkdir()
            def git(*args):
                return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True).stdout.strip()
            git("init")
            git("config", "user.email", "review@example.com")
            git("config", "user.name", "Review test")
            scripts = repo / ".gitea/scripts"
            scripts.mkdir(parents=True)
            for name in ("opencode-review.sh", "opencode-review-action.sh", "extract-opencode-review.py"):
                shutil.copy(SOURCE.with_name(name), scripts / name)
            (repo / "source.txt").write_text("common ancestor\n")
            git("add", ".")
            git("commit", "-m", "fixture")
            ancestor = git("rev-parse", "HEAD")
            git("checkout", "-b", "pr-source")
            (scripts / "opencode-review.sh").write_text("exit 99\n")
            (repo / "source.txt").write_text("PR head\n")
            (repo / "large.txt").write_text("added PR line\n" * 40_000)
            git("add", ".")
            git("commit", "-m", "PR changes")
            head = git("rev-parse", "HEAD")
            git("checkout", "-b", "release/review", ancestor)
            (repo / "target-only.txt").write_text("target branch advanced\n")
            git("add", ".")
            git("commit", "-m", "target changes")
            base = git("rev-parse", "HEAD")
            git("remote", "add", "origin", str(repo))
            git("update-ref", "refs/pull/7/head", head)

            capture = root / "capture"
            capture.mkdir()
            pr = {"state": "open", "head": {"sha": head, "ref": "pr-source"},
                  "base": {"sha": "target-moved-after-checkout", "ref": "release/review"},
                  "title": "Review fixture", "body": "PR description", "html_url": "https://example.com/pulls/7"}
            (capture / "pr.json").write_text(json.dumps(pr))
            tea = root / "tea"
            tea.write_text(
                "#!/usr/bin/env python3\n"
                "import os, pathlib, sys\n"
                "capture = pathlib.Path(os.environ['CAPTURE'])\n"
                "if '-X' in sys.argv:\n"
                "    payload = pathlib.Path(sys.argv[sys.argv.index('-d') + 1][1:])\n"
                "    (capture / 'payload.json').write_text(payload.read_text())\n"
                "elif sys.argv[2].endswith('/reviews'):\n"
                "    print('[]')\n"
                "else:\n"
                "    print((capture / 'pr.json').read_text())\n"
            )
            tea.chmod(0o755)
            executable = root / "opencode"
            executable.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, pathlib, sys\n"
                "if sys.argv[1:] == ['--version']:\n"
                "    print('fixture'); sys.exit(0)\n"
                "if sys.argv[1:] == ['run', '--help']:\n"
                "    print('--standalone'); sys.exit(0)\n"
                "capture = pathlib.Path(os.environ['CAPTURE'])\n"
                "(capture / 'prompt.txt').write_text(sys.argv[2])\n"
                "for i, arg in enumerate(sys.argv):\n"
                "    if arg == '--file':\n"
                "        path = pathlib.Path(sys.argv[i + 1])\n"
                "        (capture / path.name).write_text(path.read_text())\n"
                "review = '## 结论\\n审查完成\\n## 阻塞问题\\n未发现阻塞问题\\n## 非阻塞建议\\n无'\n"
                "print(json.dumps({'type': 'text', 'part': {'text': review}}))\n"
            )
            executable.chmod(0o755)
            subprocess.run(["bash", ".gitea/scripts/opencode-review.sh"], cwd=repo, check=True,
                           capture_output=True, text=True, env=os.environ | {
                               "PATH": f"{root}:{os.environ['PATH']}", "CAPTURE": str(capture),
                               "OPENCODE_BIN": str(executable), "REVIEW_PR_NUMBER": "7", "REVIEW_HEAD_SHA": head,
                           })
            context = (capture / "context.md").read_text()
            self.assertIn(f"- Base: `release/review` (`{base}`)", context)
            self.assertIn(f"- Merge base: `{ancestor}`", context)
            self.assertIn(f"- Head: `pr-source` (`{head}`)", context)
            self.assertIn("PR description", context)
            diff = (capture / "diff.patch").read_text()
            self.assertGreater(len(diff), 500_000)
            self.assertEqual(diff, git("diff", "--no-ext-diff", "--find-renames", f"{base}...{head}") + "\n")
            self.assertNotIn("target-only.txt", diff)
            self.assertIn("+PR head", diff)
            prompt = (capture / "prompt.txt").read_text()
            self.assertIn(f"被审查的 PR 的目标分支代码在：{repo}", prompt)
            self.assertNotIn("$workdir", prompt)
            self.assertIn("/context.md", prompt)
            self.assertIn("/diff.patch", prompt)
            self.assertEqual(git("rev-parse", "HEAD"), base)
            self.assertEqual((repo / "source.txt").read_text(), "common ancestor\n")
            self.assertEqual(json.loads((capture / "payload.json").read_text())["commit_id"], head)

    def test_run_uses_isolated_directory_and_supported_flags(self):
        for standalone in (False, True):
            with self.subTest(standalone=standalone):
                self.check_invocation(standalone)

    def check_invocation(self, standalone):
        source = SOURCE.read_text()
        block = source.split("\n(\n", 1)[1].split('\n) > "$workdir/opencode.jsonl"', 1)[0]
        help_line = 'run_help="$("$opencode_bin" run --help)"'
        setup = source.split(help_line, 1)[1].split("\nprompt=", 1)[0]
        setup = help_line + setup
        self.assertNotRegex(block, r"--(?:pure|dir)\b")
        self.assertIn('cp "$repo_root/.gitea/scripts/opencode-review-action.sh"', source)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            work = root / "review with spaces"
            work.mkdir()
            for name in ("context.md", "diff.patch"):
                (work / name).write_text("fixture")
            executable = root / "opencode"
            executable.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys\n"
                "if sys.argv[1:] == ['run', '--help']:\n"
                "    print('--standalone' if os.environ['STANDALONE'] == '1' else '--agent --format --file')\n"
                "    sys.exit(0)\n"
                "if '--standalone' in sys.argv and os.environ['STANDALONE'] != '1':\n"
                "    sys.exit(1)\n"
                "print(json.dumps({'cwd': os.getcwd(), 'args': sys.argv[1:], "
                "'repo': os.environ['REVIEW_REPO_ROOT'], "
                "'config': json.load(open('opencode.json'))}))\n"
            )
            executable.chmod(0o755)
            result = subprocess.run(
                ["bash", "-eu", "-c", setup + '\n' + block], check=True, capture_output=True, text=True,
                env=dict(os.environ, PATH=f"{root}:{os.environ['PATH']}",
                          STANDALONE="1" if standalone else "0",
                          opencode_bin=str(executable),
                         workdir=str(work), repo_root="/trusted/repository", marker="fixture",
                         approval_file=str(work / "approved"), prompt="Review fixture"),
            )
            invocation = json.loads(result.stdout)
            self.assertEqual(invocation["cwd"], str(work))
            self.assertEqual(invocation["repo"], "/trusted/repository")
            self.assertEqual(invocation["args"], [
                "run", "Review fixture", *(["--standalone"] if standalone else []), "--agent", "build",
                "--model", "openai/gpt-6.1-sol", "--format", "json",
                "--file", str(work / "context.md"), "--file", str(work / "diff.patch"),
            ])
            config = invocation["config"]
            if standalone:
                self.assertIn("permissions", config)
                self.assertNotIn("permission", config)
            else:
                self.assertNotIn("permissions", config)
                self.assertEqual(config["permission"], {"*": "allow"})
                self.assertEqual(config["agent"]["build"], {
                    "permission": {"*": "allow"},
                })

    def test_host_runner_prefers_user_installation_and_honors_override(self):
        source = SOURCE.read_text()
        selection = source.split('opencode_bin="${OPENCODE_BIN:-}"', 1)[1].split('\npr_json=', 1)[0]
        selection = 'opencode_bin="${OPENCODE_BIN:-}"' + selection
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home with spaces"
            user_bin = home / ".opencode" / "bin" / "opencode"
            user_bin.parent.mkdir(parents=True)
            system_bin = root / "opencode"
            override = root / "explicit opencode"
            for executable, version in ((user_bin, "v2.0.26"), (system_bin, "1.18.32"), (override, "custom")):
                executable.write_text(f"#!/bin/sh\necho {version}\n")
                executable.chmod(0o755)
            env = dict(os.environ, HOME=str(home), PATH=f"{root}:/usr/bin:/bin", OPENCODE_BIN="")

            def select(**changes):
                return subprocess.run(["bash", "-eu", "-c", selection],
                    capture_output=True, text=True, env=env | changes)

            result = select()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"{user_bin} (v2.0.26)", result.stdout)
            result = select(OPENCODE_BIN=str(override))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"{override} (custom)", result.stdout)
            result = select(OPENCODE_BIN=str(root / "missing"))
            self.assertEqual(result.returncode, 2)
            self.assertIn("OpenCode executable not found", result.stderr)
            user_bin.unlink()
            result = select()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"{system_bin} (1.18.32)", result.stdout)


if __name__ == "__main__":
    unittest.main()
