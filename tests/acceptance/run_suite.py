#!/usr/bin/env python3
"""安装构建产出的软件包，并对安装结果运行验收用例。

CI 调用：

    python3 -B tests/acceptance/run_suite.py --package-dir /transfer/<run>-<sha> --output target/test-records

本机调试（跳过安装，直接用已有二进制）：

    python3 -B tests/acceptance/run_suite.py --bin-dir target/release \\
        --output /tmp/test-records --case gui_panel_open_close

退出码：0 全部通过，1 有失败，2 参数或环境错误。
"""
import argparse
import os
import sys
from pathlib import Path

import install
import session
from cases import CASES, MODULES
from record import (
    STATUS_FAILED,
    STATUS_NOT_RUN,
    STATUS_PASSED,
    Context,
    read_json,
    write_json,
)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--output", type=Path, help="记录目录，必须尚不存在")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--package-dir", type=Path, help="构建产物目录（CI 使用）")
    source.add_argument("--bin-dir", type=Path, help="本机调试：跳过安装，直接使用该目录的二进制")
    parser.add_argument("--case", action="append", dest="cases", metavar="ID", help="只运行指定用例，可重复")
    parser.add_argument("--list", action="store_true", help="列出可用用例")
    parser.add_argument("--in-session", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.list:
        return args
    if args.output is None:
        parser.error("需要 --output")
    args.output = args.output.resolve()
    if args.in_session:
        if len(args.cases or []) != 1:
            parser.error("--in-session 需要且只需要一个 --case")
        return args
    if args.package_dir is None and args.bin_dir is None:
        parser.error("需要 --package-dir 或 --bin-dir")
    return args


def select(cases):
    if not cases:
        return list(CASES)
    unknown = [case_id for case_id in cases if case_id not in MODULES]
    if unknown:
        raise SystemExit(f"未知用例: {' '.join(unknown)}")
    return [MODULES[case_id].CASE for case_id in cases]


def write_case_result(directory, case, error=None):
    """按步骤记录汇总用例结果，写入用例目录的 result.json。"""
    steps = []
    for path in directory.glob("*/result.json"):
        record = read_json(path)
        steps.append({
            "name": record.get("name", path.parent.name),
            "order": record.get("order", 0),
            "status": record.get("status", STATUS_FAILED),
        })
    steps.sort(key=lambda item: item["order"])
    failed = error is not None or not steps or any(step["status"] != STATUS_PASSED for step in steps)
    record = {
        "case": case.id,
        "title": case.title,
        "status": STATUS_FAILED if failed else STATUS_PASSED,
        "steps": steps,
    }
    if error is not None:
        record["error"] = error
    write_json(directory / "result.json", record)
    return record


def mark_not_run(output, case, reason):
    directory = output / case.title
    directory.mkdir()
    write_json(directory / "result.json", {
        "case": case.id,
        "title": case.title,
        "status": STATUS_NOT_RUN,
        "reason": reason,
    })
    return {"id": case.id, "title": case.title, "status": STATUS_NOT_RUN, "reason": reason}


def run_case(case, output, bin_dir):
    directory = output / case.title
    directory.mkdir()
    if "session" in case.requires:
        session.run(case, Path(__file__).resolve(), output, bin_dir)
        if not (directory / "result.json").is_file():
            write_json(directory / "result.json", {
                "case": case.id,
                "title": case.title,
                "status": STATUS_FAILED,
                "error": "用例初始化失败；详见 session.log",
            })
    else:
        error = None
        try:
            MODULES[case.id].run(Context(directory, bin_dir))
        except Exception as exception:
            error = str(exception)
        write_case_result(directory, case, error)
    record = read_json(directory / "result.json")
    return {"id": case.id, "title": case.title, "status": record["status"]}


def run_case_in_session(args, bin_dir):
    case = MODULES[args.cases[0]].CASE
    directory = args.output / case.title
    error = None
    try:
        MODULES[case.id].run(Context(directory, bin_dir))
    except Exception as exception:
        error = str(exception)
    record = write_case_result(directory, case, error)
    print(f"{case.title}: {record['status']}", file=sys.stderr)
    return 0 if record["status"] == STATUS_PASSED else 1


def print_summary(summary):
    for case in summary["cases"]:
        print(f"{case['title']}: {case['status']}", file=sys.stderr)
    print(f"总结果: {summary['result']}", file=sys.stderr)


def main(argv=None):
    args = parse_args(argv)
    if args.list:
        for case in CASES:
            print(case.id)
        return 0
    if args.in_session:
        return run_case_in_session(args, Path(os.environ["DESKWAIFU_ACCEPTANCE_BIN_DIR"]))
    if args.output.exists():
        print(f"记录目录已存在: {args.output}", file=sys.stderr)
        return 2

    args.output.mkdir(parents=True)
    installed, install_error = None, None
    if args.package_dir is not None:
        try:
            installed = install.install(args.output / "install", args.package_dir)
        except Exception as exception:
            install_error = str(exception)
    else:
        installed = args.bin_dir.resolve()

    results = []
    for case in select(args.cases):
        if installed is None:
            results.append(mark_not_run(args.output, case, "安装阶段失败"))
        else:
            results.append(run_case(case, args.output, installed))

    passed = installed is not None and all(item["status"] == STATUS_PASSED for item in results)
    summary = {
        "result": STATUS_PASSED if passed else STATUS_FAILED,
        "install": {
            "status": STATUS_PASSED if installed is not None else STATUS_FAILED,
            "package_dir": str(args.package_dir) if args.package_dir is not None else None,
        },
        "cases": results,
    }
    if install_error is not None:
        summary["install"]["error"] = install_error
    write_json(args.output / "summary.json", summary)
    print_summary(summary)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
