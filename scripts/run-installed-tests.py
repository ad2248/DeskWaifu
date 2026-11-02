#!/usr/bin/env python3
"""Run the installed package's test registry without a source/build directory."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parent
    registry = json.loads((root / "installed-tests.json").read_text())
    if not registry["tests"]:
        raise ValueError("Empty installed test suite")
    failures = []
    for test in registry["tests"]:
        with tempfile.TemporaryDirectory(prefix="installed-test-") as temporary:
            env = dict(os.environ)
            env["OPENCV_FOR_THREADS_NUM"] = "2"
            for entry in test["environment"]:
                key, value = entry.split("=", 1)
                env[key] = value
            command = [arg.replace("@TEMP@", temporary) for arg in test["command"]]
            # This test intentionally creates a fake sibling executable. Never
            # make the package installation writable to accommodate it.
            if test["name"] == "gui-plugin-shutdown":
                executable = Path(temporary) / Path(command[0]).name
                shutil.copy2(command[0], executable)
                command = [str(executable), *command[1:]]
            try:
                result = subprocess.run(command, cwd=temporary, env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        # PRoot instruments filesystem/process syscalls. The source gate
                                        # retains the original deadline; allow that overhead here.
                                        timeout=max(60, test["timeout"] * 3))
                print(f"=== {test['name']} ===\n{result.stdout}", flush=True)
                if result.returncode:
                    if result.returncode != test["skip"] or test["name"] not in registry["optional_processes"]:
                        failures.append(test["name"])
                for skipped in re.findall(r"^SKIP\s*:\s*([^\s(]+)\(", result.stdout, re.MULTILINE):
                    if skipped not in registry["optional_cases"]:
                        failures.append(f"{test['name']}/{skipped}")
                if re.search(r"^OK \(skipped=\d+\)", result.stdout, re.MULTILINE):
                    failures.append(test["name"] + ": unexpected unittest skip")
            except subprocess.TimeoutExpired:
                failures.append(test["name"] + ": timeout")
    print(json.dumps({"count": len(registry["tests"]), "failures": failures}))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
