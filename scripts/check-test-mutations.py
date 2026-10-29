#!/usr/bin/env python3
"""Bounded behavioral mutations in an owned source copy, never the checkout."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MUTATIONS = [
    ("late-speech", "src/plugins/speech/private/processor/speech/SpeechPlayer.cpp",
     "return _id && e.id == _id;", "return bool(_id);",
     "speech-processor-contract-test", "speech-contract-replace", "old results cannot affect the new request"),
    ("hover", "src/plugins/desktop/private/processor/interaction/InteractionProcessor.cpp",
     "? hoverOpacity : idleOpacity;", "? idleOpacity : idleOpacity;",
     "interaction-processor-test", "interaction-opacity", "frame.opacity == hovered"),
    ("gpu-release", "src/plugins/desktop/private/processor/model/ModelProcessor.cpp",
     "_stoppedCleanly = _runtime->Shutdown(_target->MakeCurrent());", "_stoppedCleanly = true;",
     "model-render-thread-test", "model-render-thread", "calls.created == calls.deleted"),
    ("cursor-timer", "src/plugins/desktop/private/processor/input/InputListener.cpp",
     "_cursorTimer.start(16);", ";",
     "model-pointer-test", "model-pointer-cursor", "periodic cursor sampling must progress without presentation frames"),
    ("cursor-assembly", "src/plugins/desktop/private/app/LAppDelegate.cpp",
     "if (!_input->StartCursor())", "if (false)",
     "model-pointer-test", "model-pointer-cursor", "cursorStarts == 1"),
    ("gpu-context-guard", "src/plugins/desktop/private/processor/model/ModelRuntime.cpp",
     "if (!contextCurrent && (_view || _manager || _textures))", "if (false)",
     "model-render-thread-test", "model-shutdown-context", "calls.forbiddenDeletes == 0"),
]


def run(command, log):
    with log.open("w") as output:
        return subprocess.run(command, stdout=output, stderr=subprocess.STDOUT).returncode


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check-test-mutations.py CONFIGURED_BUILD")
    original_build = Path(sys.argv[1]).resolve()
    cache = {}
    for line in (original_build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    log_root = original_build / "test-logs/mutations"
    log_root.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix="run-", dir=log_root))
    parent = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    results = []
    with tempfile.TemporaryDirectory(prefix="deskwaifu-mutations-", dir=parent) as directory:
        source, build = Path(directory) / "source", Path(directory) / "build"
        shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns(".git", "build*", "__pycache__", "CubismSdkForNative-5-r.5", "glew"))
        configure = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                     "-DBUILD_TESTING=ON", "-DDESKWAIFU_BUILD_CONTROL=ON", "-DDESKWAIFU_TEST_GATE=ON",
                     "-DSDK_ROOT_PATH=" + cache["SDK_ROOT_PATH"], "-DGLEW_PATH=" + cache["GLEW_PATH"]]
        if run(configure, logs / "configure.log"):
            sys.exit(f"mutation configuration failed; see {logs}")
        for name, relative, old, new, target, test, assertion in MUTATIONS:
            path = source / relative
            clean = path.read_text()
            if clean.count(old) != 1:
                sys.exit(f"mutation {name}: expected exactly one anchor")
            path.write_text(clean.replace(old, new))
            try:
                compiled = run(["cmake", "--build", str(build), "--target", target, "--parallel", "8"], logs / f"{name}-build.log") == 0
                code = run([sys.executable, str(source / "scripts/run-test-gate.py"), str(build), "--test", f"^{test}$"], logs / f"{name}-test.log") if compiled else None
                text = (logs / f"{name}-test.log").read_text() if compiled else ""
                caught = compiled and code != 0 and assertion in text
                results.append({"mutation": name, "test": test, "compiled": compiled, "exit": code,
                                "expected_assertion": assertion, "caught": caught})
            finally:
                path.write_text(clean)
            # A successful restored run distinguishes behavioral interception
            # from broken fixtures, missing capability or unrelated crashes.
            restored = run(["cmake", "--build", str(build), "--target", target, "--parallel", "8"], logs / f"{name}-restored-build.log") == 0
            if restored:
                restored = run([sys.executable, str(source / "scripts/run-test-gate.py"), str(build), "--test", f"^{test}$"], logs / f"{name}-restored-test.log") == 0
            results[-1]["restored_passed"] = restored
            print(json.dumps(results[-1], ensure_ascii=False))
    (logs / "summary.json").write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n")
    print(f"Mutation report: {logs / 'summary.json'}")
    return int(not all(row["caught"] and row["restored_passed"] for row in results))


if __name__ == "__main__":
    sys.exit(main())
