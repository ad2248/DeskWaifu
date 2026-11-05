#!/usr/bin/env python3
"""Bounded behavioral mutations in an owned source copy, never the checkout.

A mutation runs when a changed path can affect its production file, the test that
must catch it, or a helper header that test target compiles. A clean tree or a
selection wide enough to touch `deskwaifu-core` cannot prove a mapping, so the full
set runs instead. Every run checks the selected mutations and nothing outside the
selection, and a partial report says so.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]

# The production targets every mutation build links, owned by paths that cannot be
# mapped to one mutation. A change there may break any expectation, so it selects all.
SHARED_OWNERS = ("CMakeLists.txt", "cmake/", "platform/", "sdk/", "thirdParty/")

# Paths that no compilation reads. Only these may skip every mutation.
INERT = ("docs/", ".gitea/", "README.md")

MUTATIONS = [
    {"mutation": "late-speech",
     "owned": ["plugins/speech/private/processor/speech/"],
     "tests": ["plugins/speech/tests/SpeechProcessorContractTest.cpp"],
     # 该测试目标编译的辅助头文件：不在生产归属目录内，但同样决定它能否通过。
     "support": ["plugins/speech/tests/EventLoopFence.hpp"],
     "relative": "plugins/speech/private/processor/speech/SpeechPlayer.cpp",
     "old": "return _id && e.id == _id;", "new": "return bool(_id);",
     "target": "speech-processor-contract-test", "test": "speech-contract-replace",
     "assertion": "old results cannot affect the new request"},
    {"mutation": "hover",
     "owned": ["plugins/desktop/private/processor/interaction/"],
     "tests": ["plugins/desktop/tests/InteractionProcessorTest.cpp"],
     "support": ["plugins/desktop/tests/EventLoopFence.hpp",
                 # interaction-processor-test 链接 deskwaifu_application，测试源码还
                 # 直接包含输入/模型/渲染的公开头文件
                 "plugins/desktop/private/processor/input/",
                 "plugins/desktop/private/processor/model/",
                 "plugins/desktop/private/render/ModelRenderTarget.hpp"],
     "relative": "plugins/desktop/private/processor/interaction/InteractionProcessor.cpp",
     "old": "? hoverOpacity : idleOpacity;", "new": "? idleOpacity : idleOpacity;",
     "target": "interaction-processor-test", "test": "interaction-opacity",
     "assertion": "frame.opacity == hovered"},
    {"mutation": "gpu-release",
     "owned": ["plugins/desktop/private/processor/model/"],
     "tests": ["plugins/desktop/tests/ModelRenderThreadTest.cpp"],
     "support": ["plugins/desktop/tests/ModelProcessorTestHelpers.hpp",
                 # model-render-thread-test 链接 deskwaifu_application，并包含窗口与渲染头文件
                 "plugins/desktop/private/processor/interaction/window/",
                 "plugins/desktop/private/render/ModelRenderTarget.hpp"],
     "relative": "plugins/desktop/private/processor/model/ModelProcessor.cpp",
     "old": "_stoppedCleanly = _runtime->Shutdown(_target->MakeCurrent());", "new": "_stoppedCleanly = true;",
     "target": "model-render-thread-test", "test": "model-render-thread",
     "assertion": "calls.created == calls.deleted"},
    {"mutation": "cursor-timer",
     "owned": ["plugins/desktop/private/processor/input/"],
     "tests": ["plugins/desktop/tests/ModelPointerTest.cpp"],
     "support": ["plugins/desktop/tests/TestApplication.hpp",
                 "plugins/desktop/tests/ModelProcessorTestHelpers.hpp",
                 "plugins/desktop/tests/DispatchedInput.hpp",
                 "plugins/desktop/tests/TestTemporaryDirectory.hpp",
                 # model-pointer-test 链接 deskwaifu_application，包含几乎整份应用
                 # 实现的公开头文件，改动任一都会改变该故障的可观测行为
                 "plugins/desktop/private/app/LAppDelegate.hpp",
                 "plugins/desktop/private/processor/interaction/",
                 "plugins/desktop/private/render/ModelRenderTarget.hpp"],
     "relative": "plugins/desktop/private/processor/input/InputListener.cpp",
     "old": "_cursorTimer.start(16);", "new": ";",
     "target": "model-pointer-test", "test": "model-pointer-cursor",
     "assertion": "periodic cursor sampling must progress without presentation frames"},
    {"mutation": "cursor-assembly",
     "owned": ["plugins/desktop/private/app/"],
     "tests": ["plugins/desktop/tests/ModelPointerTest.cpp"],
     "support": ["plugins/desktop/tests/TestApplication.hpp",
                 "plugins/desktop/tests/ModelProcessorTestHelpers.hpp",
                 "plugins/desktop/tests/DispatchedInput.hpp",
                 "plugins/desktop/tests/TestTemporaryDirectory.hpp",
                 # 同上：与 cursor-timer 共用 model-pointer-test 的依赖面
                 "plugins/desktop/private/processor/input/",
                 "plugins/desktop/private/processor/interaction/",
                 "plugins/desktop/private/processor/signal/SignalProcessor.hpp",
                 "plugins/desktop/private/render/ModelRenderTarget.hpp"],
     "relative": "plugins/desktop/private/app/LAppDelegate.cpp",
     "old": "if (!_input->StartCursor())", "new": "if (false)",
     "target": "model-pointer-test", "test": "model-pointer-cursor",
     "assertion": "cursorStarts == 1"},
    {"mutation": "gpu-context-guard",
     "owned": ["plugins/desktop/private/processor/model/"],
     "tests": ["plugins/desktop/tests/ModelRenderThreadTest.cpp"],
     "support": ["plugins/desktop/tests/ModelProcessorTestHelpers.hpp",
                 # 同上：model-render-thread-test 的窗口与渲染依赖
                 "plugins/desktop/private/processor/interaction/window/",
                 "plugins/desktop/private/render/ModelRenderTarget.hpp"],
     "relative": "plugins/desktop/private/processor/model/ModelRuntime.cpp",
     "old": "if (!contextCurrent && (_view || _manager || _textures))", "new": "if (false)",
     "target": "model-render-thread-test", "test": "model-shutdown-context",
     "assertion": "calls.forbiddenDeletes == 0"},
]


def affected_paths(mutation):
    """Every path this mutation's build reads: production, test source, helper headers."""
    return (*mutation["owned"], *mutation["tests"], *mutation.get("support", ()))


def select(changed):
    """Return the mutations a changed-path list can affect, or all when unprovable.

    Anything that is not inert may alter a shared target, so a single unmapped path
    selects every mutation. Only a diff made entirely of inert paths runs none.
    """
    if not changed:
        return list(MUTATIONS)
    if all(path.startswith(INERT) for path in changed):
        return []
    if any(path.startswith(SHARED_OWNERS) for path in changed):
        return list(MUTATIONS)
    selected = [m for m in MUTATIONS
                if any(path.startswith(tuple(affected_paths(m))) for path in changed)]
    mapped = {path for m in MUTATIONS for path in affected_paths(m)}
    unmapped = [path for path in changed
                if not path.startswith(tuple(mapped)) and not path.startswith(INERT)]
    return selected if not unmapped else list(MUTATIONS)


class UnprovableDiff(Exception):
    """The branch's own changes cannot be separated from the base branch."""


def merge_base(ref):
    """The fork point of ref and HEAD, or None when git cannot name one."""
    command = ["git", "-C", str(ROOT), "merge-base", ref, "HEAD"]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def changed_paths(base):
    """Paths this branch changed since its fork point, or None without a baseline.

    The baseline is the merge base, not the base branch tip: commits that landed on
    the base branch after the fork are not this branch's changes. `fork..HEAD` is the
    same set as `base...HEAD` once the fork point is named, but naming it removes the
    dependence on git re-deriving a merge base that a shallow graft can silently
    misreport as the clone boundary. Because of that graft, a shallow history cannot
    prove which side a path came from, so it is refused instead of guessed.
    """
    if not base:
        return None
    shallow = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--is-shallow-repository"],
                             capture_output=True, text=True)
    if shallow.returncode == 0 and shallow.stdout.strip() == "true":
        raise UnprovableDiff("the repository is a shallow clone, so its merge base is a graft boundary")
    fork = merge_base(base)
    if fork is None:
        raise UnprovableDiff(f"{base} has no merge base with HEAD")
    # 不加 quotepath=false 时 git 会把中文路径转义成带引号的八进制串，永远匹配不上映射表
    result = subprocess.run(["git", "-C", str(ROOT), "-c", "core.quotepath=false",
                             "diff", "--name-only", f"{fork}..HEAD"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise UnprovableDiff(result.stderr.strip() or f"cannot diff {fork} against HEAD")
    return [line for line in result.stdout.splitlines() if line]


def write_selection(build, record):
    """Record the selection at a fixed path; the `run-*` directory needs a run to exist.

    CI uploads evidence only on failure, so a passing run needs this file to answer
    why no mutation ran without reading the job log. The job summary gets the same
    selection because the log line alone scrolls away.
    """
    path = Path(build).resolve() / "test-logs/mutations/selection.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n")
    print(f"Mutation selection: {path} {json.dumps(record, ensure_ascii=False)}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as output:
            output.write(f"- 变异测试选择：{record['reason']}；"
                         f"已选 {len(record['selected'])} 项，改动 {record['changed']}\n")
    return path


def selection_record(base, changed, selected):
    if changed is None:
        reason = "no base commit; only the full set is provable"
    elif not selected:
        reason = "no changed path affects a mutation"
    elif len(selected) == len(MUTATIONS):
        reason = "changed paths can affect every mutation"
    else:
        reason = "only mutations affected by the changed paths ran"
    return {"full": len(selected) == len(MUTATIONS), "base": base, "changed": changed or [],
            "selected": [mutation["mutation"] for mutation in selected], "reason": reason}


def run(command, log):
    with log.open("w") as output:
        return subprocess.run(command, stdout=output, stderr=subprocess.STDOUT).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", help="a configured build directory")
    parser.add_argument("--base", help="run only mutations the diff from this commit can affect")
    arguments = parser.parse_args()
    try:
        changed = changed_paths(arguments.base)
    except UnprovableDiff as error:
        sys.exit(f"cannot select mutations for {arguments.base}: {error}; a partial run would be unproven")
    selected = select(changed)
    record = selection_record(arguments.base, changed, selected)
    if changed is not None and not selected:
        write_selection(arguments.build, record)
        print(json.dumps({"selected": 0, "reason": record["reason"], "changed": changed},
                         ensure_ascii=False))
        return 0
    full = record["full"]

    original_build = Path(arguments.build).resolve()
    cache = {}
    for line in (original_build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    log_root = original_build / "test-logs/mutations"
    log_root.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix="run-", dir=log_root))
    write_selection(original_build, record)
    parent = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    results = []
    with tempfile.TemporaryDirectory(prefix="deskwaifu-mutations-", dir=parent) as directory:
        source, build = Path(directory) / "source", Path(directory) / "build"
        shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns(".git", "build*", "__pycache__", "CubismSdkForNative-5-r.5", "glew"))
        configure = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                     "-DBUILD_TESTING=ON", "-DDESKWAIFU_BUILD_CONTROL=ON", "-DDESKWAIFU_TEST_GATE=ON",
                     "-DDESKWAIFU_BUILD_OCR=OFF",
                     "-DSDK_ROOT_PATH=" + cache["SDK_ROOT_PATH"], "-DGLEW_PATH=" + cache["GLEW_PATH"]]
        # These mutations exercise application/plugin behavior, not the optional native OCR engine.
        if run(configure, logs / "configure.log"):
            sys.exit(f"mutation configuration failed; see {logs}\n{(logs / 'configure.log').read_text()}")
        for mutation in selected:
            name, relative = mutation["mutation"], mutation["relative"]
            old, new = mutation["old"], mutation["new"]
            target, test, assertion = mutation["target"], mutation["test"], mutation["assertion"]
            path = source / relative
            clean = path.read_text()
            if clean.count(old) != 1:
                sys.exit(f"mutation {name}: expected exactly one anchor")
            path.write_text(clean.replace(old, new))
            try:
                compiled = run(["cmake", "--build", str(build), "--target", target, "--parallel", "24"], logs / f"{name}-build.log") == 0
                code = run([sys.executable, str(source / "scripts/run-test-gate.py"), str(build), "--test", f"^{test}$"], logs / f"{name}-test.log") if compiled else None
                text = (logs / f"{name}-test.log").read_text() if compiled else ""
                caught = compiled and code != 0 and assertion in text
                results.append({"mutation": name, "test": test, "compiled": compiled, "exit": code,
                                "expected_assertion": assertion, "caught": caught})
            finally:
                path.write_text(clean)
            # A successful restored run distinguishes behavioral interception
            # from broken fixtures, missing capability or unrelated crashes.
            restored = run(["cmake", "--build", str(build), "--target", target, "--parallel", "24"], logs / f"{name}-restored-build.log") == 0
            if restored:
                restored = run([sys.executable, str(source / "scripts/run-test-gate.py"), str(build), "--test", f"^{test}$"], logs / f"{name}-restored-test.log") == 0
            results[-1]["restored_passed"] = restored
            print(json.dumps(results[-1], ensure_ascii=False))
    summary = {**record, "results": results}
    (logs / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
    print(f"Mutation report: {logs / 'summary.json'}")
    if not full:
        print("PARTIAL: only mutations affected by the changed paths ran; "
              "this report is not evidence for the full set")
    return int(not all(row["caught"] and row["restored_passed"] for row in results))


if __name__ == "__main__":
    sys.exit(main())
