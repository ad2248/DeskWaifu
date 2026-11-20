#!/usr/bin/env python3
"""Resolve the explicit acceptance reference in a PR description."""

import json
import re
import subprocess
import sys


MAX_DOCUMENT_BYTES = 128 * 1024


def command(*args):
    return subprocess.run(args, check=True, capture_output=True).stdout


def document(path, sha):
    if (not path.lower().endswith(".md") or
            any(part in ("", ".", "..") for part in path.split("/")) or
            any(ord(char) < 32 for char in path)):
        raise ValueError("请填写仓库内的 Markdown 相对路径。")
    entry = command("git", "--literal-pathspecs", "ls-tree", "-z", sha, "--", path)
    if not entry:
        raise ValueError(f"所选提交 {sha} 中找不到方案文档。")
    mode, kind, oid = entry.split(b"\t", 1)[0].decode("ascii").split()
    if kind != "blob" or mode not in ("100644", "100755"):
        raise ValueError("方案引用必须指向普通 Markdown 文件。")
    if int(command("git", "cat-file", "-s", oid)) > MAX_DOCUMENT_BYTES:
        raise ValueError("方案文档超过 128 KiB，请引用更小的验收文档。")
    text = command("git", "cat-file", "blob", oid).decode("utf-8")
    if not text.strip():
        raise ValueError("方案文档为空。")
    return text


def material(body, repo, base_sha, head_sha):
    # 模板在首行提供此字段，旧 PR 可以省略。
    field = re.match(r"验收依据[ \t]*[:：][ \t]*([^\r\n]*)", body.lstrip())
    ref = field.group(1).strip() if field else ""
    if not ref:
        return "来源为 PR 描述。核对其中的目标和预期行为。"

    issue = re.fullmatch(r"#([1-9][0-9]*)", ref)
    if issue:
        number = issue[1]
        try:
            data = json.loads(command("tea", "api", f"/repos/{repo}/issues/{number}", "--login", "reviewer", "--repo", repo))
        except (subprocess.CalledProcessError, ValueError) as exc:
            raise ValueError(f"无法读取本仓库 Issue #{number}，请核对编号和访问权限。") from exc
        if not isinstance(data, dict) or data.get("pull_request") is not None or data.get("number") != int(number):
            raise ValueError(f"#{number} 未指向本仓库的 Issue。")
        updated = data.get("updated_at") or "未提供"
        return f"来源为本仓库 Issue #{number}（更新时间 {updated}）。\n\n{data.get('title') or ''}\n\n{data.get('body') or ''}"

    version, separator, path = ref.partition(":")
    if not separator or version not in ("base", "head"):
        raise ValueError("请使用 base:仓库相对路径、head:仓库相对路径或 #Issue编号。")
    sha = base_sha if version == "base" else head_sha
    return f"来源为仓库文档 {path}（{version} 提交 {sha}）。\n\n{document(path, sha)}"


def main():
    repo, base_sha, head_sha = sys.argv[1:]
    print("## 验收材料（审查脚本解析）\n")
    try:
        context = material(sys.stdin.read(), repo, base_sha, head_sha)
    except subprocess.CalledProcessError:
        print("验收依据无法解析。读取 Git 对象失败。")
        return 3
    except UnicodeDecodeError:
        print("验收依据无法解析。方案文档必须使用 UTF-8 编码。")
        return 3
    except ValueError as exc:
        print(f"验收依据无法解析。{exc}")
        return 3
    print(context)
    return 0


if __name__ == "__main__":
    sys.exit(main())
