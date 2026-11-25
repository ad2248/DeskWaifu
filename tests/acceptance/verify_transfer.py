#!/usr/bin/env python3
"""校验构建作业写进共享目录的软件包属于当前提交，并打印包路径与摘要。

用法：``verify_transfer.py <传输目录> <提交>``
退出码：0 校验通过，1 缺失或不符合，2 用法错误。
"""
import hashlib
import json
import sys
from pathlib import Path


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(directory, commit):
    directory = Path(directory)
    ready_path = directory / "ready.json"
    if not ready_path.is_file():
        raise SystemExit(f"缺少握手文件: {ready_path}")
    ready = json.loads(ready_path.read_text(encoding="utf-8"))
    if ready.get("sha") != commit:
        raise SystemExit(f"传输目录不属于当前提交: {ready.get('sha')} != {commit}")
    package = directory / str(ready.get("package", ""))
    if not package.is_file():
        raise SystemExit(f"缺少软件包: {package}")
    digest = sha256(package)
    if digest != ready.get("sha256"):
        raise SystemExit(f"软件包摘要不符: {digest} != {ready.get('sha256')}")
    return package, digest


def main(argv):
    if len(argv) != 2:
        print("用法: verify_transfer.py <传输目录> <提交>", file=sys.stderr)
        return 2
    package, digest = verify(argv[0], argv[1])
    print(f"{package}\t{digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
