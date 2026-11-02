#!/usr/bin/env python3
"""Run the installed package's test registry without a source/build directory."""
import json
from concurrent.futures import ThreadPoolExecutor
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
    def run_test(test):
        failed = []
        output = ""
        with tempfile.TemporaryDirectory(prefix="installed-test-") as temporary:
            env = dict(os.environ)
            # PRoot 的系统调用开销预算不改变源码门禁；外层另留清理宽限。
            timeout = max(60, test["timeout"] * 3)
            env["DESKWAIFU_TEST_TIMEOUT"] = str(timeout)
            env["DESKWAIFU_TEST_WILL_FAIL"] = "1" if test.get("will_fail", False) else "0"
            env["DESKWAIFU_TEST_SKIP_CODE"] = "" if test["skip"] is None else str(test["skip"])
            command = [arg.replace("@TEMP@", temporary) for arg in test["command"]]
            # This test intentionally creates a fake sibling executable. Never
            # make the package installation writable to accommodate it.
            if test["name"] == "gui-plugin-shutdown":
                executable = Path(temporary) / Path(command[0]).name
                shutil.copy2(command[0], executable)
                command = [str(executable), *command[1:]]
            command = [sys.executable, str(root / "test-environment.py"), "cmake", "-E", "env",
                       "OPENCV_FOR_THREADS_NUM=2", *test["environment"], *command]
            try:
                result = subprocess.run(command, cwd=temporary, env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=timeout + 10)
                output = f"=== {test['name']} ===\n{result.stdout}"
                if result.returncode:
                    if result.returncode != test["skip"] or test["name"] not in registry["optional_processes"]:
                        failed.append(test["name"])
                for skipped in re.findall(r"^SKIP\s*:\s*([^\s(]+)\(", result.stdout, re.MULTILINE):
                    if skipped not in registry["optional_cases"]:
                        failed.append(f"{test['name']}/{skipped}")
                if re.search(r"^OK \(skipped=\d+\)", result.stdout, re.MULTILINE):
                    failed.append(test["name"] + ": unexpected unittest skip")
            except subprocess.TimeoutExpired:
                failed.append(test["name"] + ": timeout")
        return output, failed

    with ThreadPoolExecutor(max_workers=12) as executor:
        for output, failed in executor.map(run_test, registry["tests"]):
            print(output, flush=True)
            failures.extend(failed)
    print(json.dumps({"count": len(registry["tests"]), "failures": failures}))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
