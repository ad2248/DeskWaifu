#!/usr/bin/env python3
"""Run CTest once; fail closed on missing protection and unexpected internal skips."""

import argparse
import json
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]


def evaluate(registry, report, policy, pattern=None):
    selected = lambda name: pattern is None or re.search(pattern, name) is not None
    names = {test["name"] for test in registry["tests"] if selected(test["name"])}
    errors = [f"required test not registered: {name}" for name in policy["required"]
              if selected(name) and name not in names]
    results = []
    seen = set()
    for test in report.iter("testcase"):
        name = test.attrib["name"]
        seen.add(name)
        output = test.findtext("system-out", "")
        if "[This part of the test output was removed" in output:
            errors.append(f"truncated output cannot prove internal case coverage: {name}")
        skipped_cases = re.findall(r"^SKIP\s*:\s*([^\s(]+)\(", output, re.MULTILINE)
        failed = test.find("failure") is not None or test.find("error") is not None
        skipped = test.find("skipped") is not None or test.get("status") in ("notrun", "disabled")
        process_status = "failed" if failed else "unverified" if skipped else "passed"
        status = "partial" if skipped_cases and process_status == "passed" else process_status
        if skipped and name not in policy["optional_processes"]:
            errors.append(f"required test skipped: {name}")
        if failed:
            errors.append(f"test failed: {name}")
        for case in skipped_cases:
            if case not in policy["optional_cases"]:
                errors.append(f"unexpected QTest skip: {name}/{case}")
        results.append({"name": name, "status": status, "process_status": process_status, "seconds": float(test.get("time", 0)),
                        "unverified_cases": [{"case": case, "reason": policy["optional_cases"].get(case, "unexpected skip")}
                                             for case in skipped_cases],
                        "reason": policy["optional_processes"].get(name, "") if skipped else ""})
    errors.extend(f"registered test produced no result: {name}" for name in sorted(names - seen))
    if not names:
        errors.append("no tests selected")
    return {"tests": results, "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--test", help="single scenario/group regex (does not claim a full gate)")
    args = parser.parse_args()
    build = args.build.resolve()
    log_root = build / "test-logs"
    log_root.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix="ctest-", dir=log_root))
    policy = json.loads((ROOT / "tests/gate.json").read_text())
    if re.search(r"^DESKWAIFU_ENABLE_SANITIZERS:BOOL=ON$", (build / "CMakeCache.txt").read_text(), re.MULTILINE):
        policy["required"] += policy["sanitizer_required"]
    registry = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    (logs / "registry.json").write_text(json.dumps(registry, indent=2))
    report = logs / "ctest.xml"
    report.unlink(missing_ok=True)
    command = ["ctest", "--test-dir", str(build), "--output-on-failure", "--no-tests=error",
               "--test-output-size-passed", "10485760", "--test-output-size-failed", "10485760",
               "--output-junit", str(report)]
    if args.test:
        command += ["-R", args.test]
    started = time.monotonic()
    with (logs / "ctest.log").open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if report.is_file():
        summary = evaluate(registry, ET.parse(report).getroot(), policy, args.test)
    else:
        summary = {"tests": [], "errors": ["CTest produced no JUnit report"]}
    if result.returncode:
        summary["errors"].append(f"CTest exit status: {result.returncode}")
    summary.update(command=command, scope=args.test or "full ordinary gate", seconds=time.monotonic() - started,
                   platform=platform.platform(), python=platform.python_version())
    (logs / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
    print((logs / "ctest.log").read_text())
    for test in summary["tests"]:
        if test["status"] == "unverified":
            print(f"UNVERIFIED {test['name']}: {test['reason']}")
        for case in test["unverified_cases"]:
            print(f"UNVERIFIED {test['name']}/{case['case']}: {case['reason']}")
    for error in summary["errors"]:
        print(f"GATE ERROR: {error}", file=sys.stderr)
    print(f"Gate report: {logs / 'summary.json'}")
    return int(bool(summary["errors"]))


if __name__ == "__main__":
    sys.exit(main())
