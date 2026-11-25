"""验收记录原语：用例元数据、步骤上下文管理器、JSON 读写。

记录目录结构固定为 ``<输出>/<用例标题>/<步骤名>/result.json``，
每一步的日志写在同目录下，便于整体打包后人工查看。
"""
import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

STATUS_NOT_RUN = "not_run"
STATUS_PASSED = "passed"
STATUS_FAILED = "failed"


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


@dataclass(frozen=True)
class Case:
    """一个验收用例。

    实现模块需提供 ``CASE`` 与 ``run(context)``；``requires`` 声明所需资源，
    ``session`` 表示由 ``session.py`` 在私有 Plasma 会话中重新调用本用例。
    """

    id: str
    title: str
    requires: frozenset = frozenset()


class Context:
    """用例拿到的上下文：记录目录与已安装文件的位置。"""

    def __init__(self, directory, bin_dir):
        self.directory = directory
        self.bin_dir = bin_dir

    def bin(self, name):
        path = Path(self.bin_dir) / name
        if not path.is_file():
            raise RuntimeError(f"缺少可执行文件: {path}")
        if not os.access(path, os.X_OK):
            raise RuntimeError(f"文件不可执行: {path}")
        return path

    def step(self, order, name, expected, action=None):
        return Step(self.directory, order, name, expected, action or {})


class Step:
    """记录一个用户可见的步骤：输入、预期、日志与结果。"""

    def __init__(self, parent, order, name, expected, action):
        self.directory = Path(parent) / name
        self.name = name
        self.output = None
        self.report = {
            "order": order,
            "name": name,
            "input": action,
            "expected": expected,
            "status": STATUS_NOT_RUN,
        }

    def __enter__(self):
        self.directory.mkdir()
        write_json(self.directory / "result.json", self.report)
        return self

    def open(self, name, mode="wb"):
        return (self.directory / name).open(mode)

    def log(self, name, data):
        path = self.directory / name
        if isinstance(data, bytes):
            path.write_bytes(data)
        else:
            path.write_text(data, encoding="utf-8")

    def __exit__(self, error_type, error, traceback):
        if self.output is not None:
            self.report["output"] = self.output
        if error is None:
            self.report["status"] = STATUS_PASSED
        else:
            self.report["status"] = STATUS_FAILED
            self.report["error"] = str(error)
            if isinstance(error, subprocess.CalledProcessError):
                self.report["exit_code"] = error.returncode
        write_json(self.directory / "result.json", self.report)
        return False
