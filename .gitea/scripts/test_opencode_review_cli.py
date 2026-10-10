"""Exercise the review invocation without contacting a model or posting reviews."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).with_name("opencode-review.sh")


class ReviewCliTest(unittest.TestCase):
    def test_v2_permissions_deny_everything_except_controlled_shell(self):
        source = SOURCE.read_text()
        config = json.loads(source.split("<<'JSON'\n", 1)[1].split("\nJSON", 1)[0])
        deny = {"action": "*", "resource": "*", "effect": "deny"}
        self.assertEqual(config["permissions"], [deny])
        self.assertEqual(config["agents"]["build"]["permissions"], [deny, {
            "action": "shell",
            "resource": ".gitea/scripts/opencode-review-action.sh *",
            "effect": "allow",
        }])

    def test_run_uses_isolated_directory_and_supported_flags(self):
        for standalone in (False, True):
            with self.subTest(standalone=standalone):
                self.check_invocation(standalone)

    def check_invocation(self, standalone):
        source = SOURCE.read_text()
        block = source.split("\n(\n", 1)[1].split('\n) > "$workdir/opencode.jsonl"', 1)[0]
        setup = source.split('run_help="$(opencode run --help)"', 1)[1].split("\nprompt=", 1)[0]
        setup = 'run_help="$(opencode run --help)"' + setup
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
                         workdir=str(work), repo_root="/trusted/repository", marker="fixture",
                         approval_file=str(work / "approved"), prompt="Review fixture"),
            )
            invocation = json.loads(result.stdout)
            self.assertEqual(invocation["cwd"], str(work))
            self.assertEqual(invocation["repo"], "/trusted/repository")
            self.assertEqual(invocation["args"], [
                "run", "Review fixture", *(["--standalone"] if standalone else []), "--agent", "build",
                "--model", "openai/gpt-6-luna", "--format", "json",
                "--file", str(work / "context.md"), "--file", str(work / "diff.patch"),
            ])
            config = invocation["config"]
            if standalone:
                self.assertIn("permissions", config)
                self.assertNotIn("permission", config)
            else:
                self.assertNotIn("permissions", config)
                self.assertEqual(config["permission"], {"*": "deny"})
                self.assertEqual(config["agent"]["build"], {
                    "steps": 8,
                    "permission": {
                        "*": "deny",
                        "bash": {
                            "*": "deny",
                            ".gitea/scripts/opencode-review-action.sh *": "allow",
                        },
                    },
                })


if __name__ == "__main__":
    unittest.main()
