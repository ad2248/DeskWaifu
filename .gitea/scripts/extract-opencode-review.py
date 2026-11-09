#!/usr/bin/env python3
"""取 OpenCode 审查会话的最后一条消息，原文作为评审正文。

模型会在工具调用之间输出过程叙述（“先确认基线”“开始核验并批准”等），
且每条消息是一组 text part。只发布最后一条消息，既不会把叙述混进评审，
也不需要校验标题格式或截断正文。
"""
import json
import sys
from pathlib import Path


def extract_review(source: Path) -> str:
    messages: dict[str, list[str]] = {}
    order: list[str] = []
    with source.open(encoding="utf-8") as stream:
        for line in stream:
            event = json.loads(line)
            if event.get("type") != "text":
                continue
            part = event.get("part") or {}
            text = part.get("text") or ""
            if not text:
                continue
            # 缺少 messageID 的旧版输出把每个 text part 当作独立消息，仍然取最后一段。
            key = part.get("messageID") or f"part-{len(order)}"
            if key not in messages:
                messages[key] = []
                order.append(key)
            messages[key].append(text)

    if not order:
        raise ValueError("OpenCode returned no review text")

    return "\n".join(messages[order[-1]])


def main() -> None:
    source, destination = map(Path, sys.argv[1:])
    try:
        review = extract_review(source)
    except (json.JSONDecodeError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
    destination.write_text(review, encoding="utf-8")


if __name__ == "__main__":
    main()
