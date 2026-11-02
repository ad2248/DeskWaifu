#!/usr/bin/env python3
"""Export executable runtime tests; source/configure gates stay in source CI."""
import argparse
import json
from pathlib import Path
import subprocess
import shutil

PORTABLE_SCRIPTS = {
    "BackendCapabilitiesTest.py", "PluginCommandTest.py", "CheckProcessorPipeline.cmake",
    "CheckPortableTemporaryDirectory.cmake", "test_ocr_native_models.py",
}


def prepare(build, source, tools, ocr=False):
    root = "/usr/lib/deskwaifu/" + ("ocr-tests" if ocr else "tests")
    mapping = {entry["build"]: entry["installed"]
               for entry in json.loads((build / "test-targets.json").read_text()) if entry}
    scripts = build / "installed-scripts"
    scripts.mkdir()
    for name in PORTABLE_SCRIPTS | {"test-environment.py"}:
        matches = list(source.rglob(name))
        if matches:
            original = matches[0]
            shutil.copy2(original, scripts / name)
            mapping[str(original)] = f"{root}/scripts/{name}"
    # Export only runtime commands. Any remaining reference to source or build
    # input is classified explicitly as a source gate, never silently executed
    # against a tree that happens to remain on the build host.
    mapping[str(build / "plugins")] = "/usr/lib/deskwaifu/plugins"
    tests, excluded = [], []
    registry = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    for test in registry["tests"]:
        properties = {item["name"]: item["value"] for item in test.get("properties", [])}
        content = json.dumps({"command": test.get("command", []),
                              "environment": properties.get("ENVIRONMENT", [])}, ensure_ascii=False)
        for old in sorted(mapping, key=len, reverse=True):
            content = content.replace(old, mapping[old])
        item = json.loads(content)
        item["command"] = ["-DTEST_TEMP_ROOT=@TEMP@/fixture" if arg.startswith("-DTEST_TEMP_ROOT=") else arg
                           for arg in item["command"]]
        content = json.dumps(item)
        if str(source) in content or str(build) in content or not test.get("command"):
            if test.get("command") and test["command"][0] in mapping:
                raise ValueError(f"Runtime test still depends on source/build paths: {test['name']}")
            excluded.append(test["name"])
            continue
        item.update(name=test["name"], timeout=properties.get("TIMEOUT", 60),
                    skip=properties.get("SKIP_RETURN_CODE"))
        tests.append(item)
    if not tests:
        raise ValueError("No installable runtime tests exported")
    policy = json.loads((tools / "../tests/gate.json").resolve().read_text())
    (build / "installed-tests.json").write_text(json.dumps({"tests": tests, "source_gates": excluded,
        "optional_processes": policy["optional_processes"], "optional_cases": policy["optional_cases"]},
        sort_keys=True, indent=2) + "\n")
    print(f"Exported {len(tests)} runtime tests; {len(excluded)} source/build gates remain in source CI")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("tools", type=Path)
    parser.add_argument("--ocr", action="store_true")
    args = parser.parse_args()
    prepare(args.build.resolve(), args.source.resolve(), args.tools.resolve(), args.ocr)
