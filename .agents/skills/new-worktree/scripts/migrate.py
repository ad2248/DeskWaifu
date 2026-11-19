#!/usr/bin/env python3
"""Move selected Git changes between worktrees without flattening the index."""

import argparse
import hashlib
import os
from pathlib import Path
import stat
import subprocess
import sys


class Unsafe(RuntimeError):
    pass


def git(root, *args, data=None, allow_missing=False):
    result = subprocess.run(
        ["git", "-C", root, "--no-optional-locks", "--literal-pathspecs", *args], input=data,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if allow_missing and result.returncode == 1 and not result.stderr:
        return b""
    if result.returncode:
        raise Unsafe(f"{root}: git {args[0]}: {os.fsdecode(result.stderr).strip()}")
    return result.stdout


def records(data):
    return data.split(b"\0")[:-1]


def line(data):
    return os.fsdecode(data.removesuffix(b"\n"))


def identity(root):
    return (
        line(git(root, "symbolic-ref", "--quiet", "HEAD", allow_missing=True)),
        line(git(root, "rev-parse", "--verify", "HEAD^{commit}")),
    )


def repo_path(root, *args):
    return Path(line(git(root, "rev-parse", "--path-format=absolute", *args))).resolve()


def root_path(value):
    if not Path(value).is_absolute():
        raise Unsafe(f"需要工作树的绝对路径：{value!r}")
    root = Path(value).resolve(strict=True)
    if root != repo_path(root, "--show-toplevel"):
        raise Unsafe(f"不是工作树根目录：{root}")
    for marker in ("MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge",
                   "rebase-apply", "sequencer", "index.lock"):
        marker_path = repo_path(root, "--git-path", marker)
        if os.path.lexists(marker_path):
            raise Unsafe(f"{root}: Git 操作尚未结束：{marker}")
    return root


def index(root):
    entries = {}
    for record in records(git(root, "ls-files", "-v", "--stage", "-z")):
        fields, path = record.split(b"\t", 1)
        tag, mode, oid, stage = fields.split()
        if tag != b"H" or stage != b"0" or path in entries:
            raise Unsafe(f"{root}: 不支持特殊索引状态：{os.fsdecode(path)!r}")
        entries[path] = (mode, oid)
    return entries


def tree(root, commit):
    entries = {}
    for record in records(git(root, "ls-tree", "-r", "-z", commit)):
        fields, path = record.split(b"\t", 1)
        mode, _, oid = fields.split()
        entries[path] = (mode, oid)
    return entries


def diff(root, *, staged):
    options = ["--cached"] if staged else []
    fields = iter(records(git(
        root, "-c", "core.fileMode=true", "diff", *options,
        "--name-status", "-z", "--find-renames", "-l0", "--ignore-submodules=none",
        "--ita-invisible-in-index", "--no-ext-diff", "--",
    )))
    changes = []
    for kind in fields:
        paths = [next(fields)]
        if kind.startswith(b"R"):
            paths.append(next(fields))
        elif kind not in (b"A", b"D", b"M", b"T"):
            raise Unsafe(f"{root}: 不支持的 Git 差异 {kind!r}：{os.fsdecode(paths[0])!r}")
        changes.append(paths)
    return changes


def leaf(root, path):
    """Never follow a Git path through symlink parents or nested repositories."""
    here = root
    parts = os.fsdecode(path).split("/")
    for part in parts:
        if part.lower() in ("", ".", "..", ".git"):
            raise Unsafe(f"{root}: 非法迁移路径 {os.fsdecode(path)!r}")
        if here != root and (here.is_symlink() or (here.exists() and not here.is_dir())
                             or os.path.lexists(here / ".git")):
            raise Unsafe(f"{root}: 路径经过非目录、符号链接或嵌套 Git 工作区：{here}")
        here /= part
    return here


def file_state(path):
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(mode):
        return ("link", stat.S_IMODE(mode), os.readlink(os.fsencode(path)))
    if stat.S_ISREG(mode):
        return ("file", stat.S_IMODE(mode), path.read_bytes())
    raise Unsafe(f"不支持目录冲突、嵌套 Git 工作区或特殊文件：{path!s}")


def capture(root, scope, *, allow_empty=False):
    head = identity(root)
    baseline, staged = tree(root, head[1]), index(root)
    staged_groups = diff(root, staged=True)
    staged_changes = {p for group in staged_groups for p in group}
    unstaged_groups = diff(root, staged=False)
    untracked = set()
    for path in records(git(root, "ls-files", "--others", "--exclude-standard", "-z")):
        if path.endswith(b"/") and os.path.lexists(leaf(root, path[:-1]) / ".git"):
            continue
        untracked.add(path)
    groups = staged_groups + unstaged_groups + [[p] for p in sorted(untracked)]
    paths = {p for group in groups for p in group}
    selected = set() if scope else paths
    for prefix in scope:
        matches = {p for p in paths if p == prefix or p.startswith(prefix + b"/")}
        if not matches and not allow_empty:
            raise Unsafe(f"{root}: 迁移范围内没有可迁移的改动：{os.fsdecode(prefix)!r}")
        selected.update(matches)
    for group in groups:
        if selected.intersection(group) and not selected.issuperset(group):
            raise Unsafe(f"{root}: 重命名必须同时迁移两端：{[os.fsdecode(p) for p in group]!r}")
    unstaged = {p for group in unstaged_groups for p in group}
    changes = {}
    for path in sorted(selected):
        old, new = baseline.get(path), staged.get(path)
        if any(entry and entry[0] == b"160000" for entry in (old, new)):
            raise Unsafe(f"{root}: 不迁移子模块：{os.fsdecode(path)!r}")
        if (old != new) != (path in staged_changes):
            raise Unsafe(f"{root}: 不支持 intent-to-add 等特殊索引状态：{os.fsdecode(path)!r}")
        state = file_state(leaf(root, path))
        if state is not None and new is None and path not in untracked:
            raise Unsafe(f"{root}: 跟踪路径上存在忽略文件：{os.fsdecode(path)!r}")
        if state is None and path in untracked:
            raise Unsafe(f"{root}: 未跟踪文件已消失：{os.fsdecode(path)!r}")
        changes[path] = (old, new, path in unstaged, path in untracked, state)
    return head, changes


def fingerprint(snapshot):
    # The snapshot is made only of ordered dicts, tuples, scalars and byte strings.
    return hashlib.sha256(repr(snapshot).encode("ascii", "backslashreplace")).hexdigest()


def write_state(path, before, after):
    if before == after:
        return
    if before is not None:
        path.unlink()
    if after is None:
        return
    kind, mode, content = after
    path.parent.mkdir(parents=True, exist_ok=True)
    if kind == "link":
        path.symlink_to(os.fsdecode(content))
    else:
        with path.open("xb") as stream:
            stream.write(content)
        path.chmod(mode)


def migrate(source, target, branch, base, token, scope):
    if source == target or repo_path(source, "--git-common-dir") != repo_path(target, "--git-common-dir"):
        raise Unsafe("需要同一仓库中两个不同的工作树")
    expected = (f"refs/heads/{branch}", base)
    head, changes = capture(source, scope)
    if fingerprint((head, changes)) != token:
        raise Unsafe(f"原工作树快照已变化：{source}")
    if capture(target, []) != (expected, {}):
        raise Unsafe(f"目标分支、基线或干净状态不符合要求：{target}")

    print("预检目标", file=sys.stderr)
    baseline = tree(target, base)
    before = {}
    updates = []
    for path, (old, new, _, _, _) in changes.items():
        dest = leaf(target, path)
        if baseline.get(path) != old:
            raise Unsafe(f"目标存在同名基线条目或迁移路径的基线不同：{dest}")
        before[path] = file_state(dest)
        if old is None and before[path] is not None:
            raise Unsafe(f"目标已存在（包括忽略文件）：{dest}")
        if old != new:
            entry = b" ".join(new) if new else b"0 " + b"0" * len(base)
            updates.append(entry + b"\t" + path + b"\0")

    print("复制并核验目标；尚未清理原工作树", file=sys.stderr)
    if updates:
        git(target, "update-index", "-z", "--index-info", data=b"".join(updates))
    for path, (_, _, _, _, state) in changes.items():
        dest = leaf(target, path)
        if file_state(dest) != before[path]:
            raise Unsafe(f"目标路径在迁移期间发生变化：{dest}")
        write_state(dest, before[path], state)
    target_head, copied = capture(target, [])
    if target_head != expected:
        raise Unsafe(f"目标分支或基线在迁移期间变化：{target}")
    if copied != changes:
        mismatches = [os.fsdecode(path) for path in sorted(copied.keys() | changes.keys())
                      if copied.get(path) != changes.get(path)]
        raise Unsafe(f"目标索引、工作区或未跟踪文件与快照不一致：{target}: {mismatches!r}")
    if capture(source, scope) != (head, changes):
        raise Unsafe(f"原工作树在迁移期间发生变化：{source}")

    print("清理原工作树；目标已核验", file=sys.stderr)
    tracked = []
    for path, (old, new, _, untracked, state) in changes.items():
        original = leaf(source, path)
        if file_state(original) != state:
            raise Unsafe(f"原路径在清理期间发生变化：{original}")
        if untracked:
            original.unlink()
        if old is not None or new is not None:
            tracked.append(path)
    if tracked:
        git(source, "restore", "--source", head[1], "--staged", "--worktree",
            "--pathspec-from-file=-", "--pathspec-file-nul", data=b"\0".join(tracked) + b"\0")
    source_head, remaining = capture(source, list(changes), allow_empty=True)
    if source_head != head or remaining or capture(target, []) != (expected, changes):
        raise Unsafe("清理后的状态核验失败")
    print(f"已迁移并核验 {len(changes)} 个路径")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("snapshot", "move"):
        command = sub.add_parser(name)
        command.add_argument("source", help="absolute source worktree root")
        if name == "move":
            command.add_argument("target", help="absolute target worktree root")
            command.add_argument("branch", help="target branch without refs/heads/")
            command.add_argument("base", help="full resolved target baseline commit ID")
            command.add_argument("token", help="output from snapshot")
        command.add_argument("paths", nargs="*", help="literal source-relative paths; omit for all")
    args = parser.parse_args()
    roots = []
    try:
        local_env = git(Path.cwd(), "rev-parse", "--local-env-vars").decode().splitlines()
        if set(local_env) & os.environ.keys():
            raise Unsafe("请先取消设置 Git 仓库局部环境变量")
        source = root_path(args.source)
        roots.append(source)
        scope = [os.fsencode(p.rstrip("/")) for p in args.paths]
        for path in scope:
            leaf(source, path)
        if args.command == "snapshot":
            print(fingerprint(capture(source, scope)))
        else:
            target = root_path(args.target)
            roots.append(target)
            migrate(source, target, args.branch, args.base, args.token, scope)
    except (Unsafe, OSError) as error:
        print(f"迁移已停止：{error}", file=sys.stderr)
        for root in roots:
            result = subprocess.run(["git", "-C", root, "--no-optional-locks", "status",
                                     "--short", "--untracked-files=all"],
                                    stdout=subprocess.PIPE, check=False)
            print(f"{root}:\n{os.fsdecode(result.stdout)}", file=sys.stderr, end="")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
