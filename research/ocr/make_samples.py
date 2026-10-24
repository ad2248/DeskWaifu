"""Generate non-private synthetic fixtures, not representative accuracy benchmarks."""

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, required=True, help="CJK-capable font file")
    parser.add_argument("--output", type=Path, default=Path("output/samples"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixtures = {}
    for name, size, background, foreground in (
        ("light", 28, "white", "black"),
        ("dark-small", 16, "#202124", "#e8eaed"),
        ("dense", 18, "white", "black"),
        ("blank", 28, "white", "black"),
    ):
        texts = ["姐姐，你好！桌宠 OCR 本地测试。", "DeskWaifu OCR 1234567890", "CPU only / no cloud upload"]
        if name == "blank":
            texts = []
        elif name == "dense":
            texts *= 6
        font = ImageFont.truetype(str(args.font), size)
        image = Image.new("RGB", (960, 640), background)
        draw = ImageDraw.Draw(image)
        for index, text in enumerate(texts):
            draw.text((24, 20 + index * (size + 10)), text, font=font, fill=foreground)
        image.save(args.output / f"{name}.png")
        if name == "light":
            image.save(args.output / "light.jpg")
        fixtures[f"{name}.png"] = texts
    (args.output / "expected.json").write_text(
        json.dumps(fixtures, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    Image.new("RGB", (4097, 1), "white").save(args.output / "oversized.png")
    Image.new("RGB", (16, 16), "white").save(
        args.output / "animated.gif", save_all=True,
        append_images=[Image.new("RGB", (16, 16), "black")],
    )


if __name__ == "__main__":
    main()
