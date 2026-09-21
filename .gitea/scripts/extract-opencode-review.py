#!/usr/bin/env python3
import json
import sys
from pathlib import Path


def extract_review(source: Path) -> str:
    parts = []
    with source.open(encoding="utf-8") as stream:
        for line in stream:
            event = json.loads(line)
            if event.get("type") == "text":
                text = event.get("part", {}).get("text", "")
                if text:
                    parts.append(text)

    review = "\n".join(parts).strip()
    if not review:
        raise ValueError("OpenCode returned no review text")

    if len(review) > 60_000:
        review = review[:60_000] + "\n\n[评审内容因长度限制被截断]"

    headings = ("## 结论", "## 阻塞问题", "## 非阻塞建议")
    lines = review.splitlines()
    positions = [lines.index(heading) if heading in lines else -1 for heading in headings]
    if positions[0] != 0 or any(position < 0 for position in positions) or positions != sorted(positions):
        raise ValueError(
            "OpenCode returned an incomplete review; expected ordered headings: "
            + ", ".join(headings)
        )

    return review


def main() -> None:
    source, destination = map(Path, sys.argv[1:])
    try:
        review = extract_review(source)
    except (json.JSONDecodeError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
    destination.write_text(review, encoding="utf-8")


if __name__ == "__main__":
    main()
