#!/usr/bin/env python3
"""Propose a terminal section from a legacy DeskWaifu desktop config."""

import argparse
import json
import math
import os
from pathlib import Path
import re
import stat
import sys


MAX_BYTES = 1024 * 1024
I32_MAX = (1 << 31) - 1
F32_MAX = float.fromhex("0x1.fffffep127")


def strip_line_comments(text):
    # The archived ConfigStore accepts // comments. Skip escaped characters
    # inside strings and preserve offsets for JSON parser diagnostics.
    chunks = []
    start = cursor = 0
    quoted = False
    while cursor < len(text):
        char = text[cursor]
        if char == '"':
            quoted = not quoted
        elif quoted and char == "\\":
            cursor += 2
            continue
        elif not quoted and char == "/" and text.startswith("//", cursor):
            end = text.find("\n", cursor)
            if end == -1:
                end = len(text)
            chunks.extend((text[start:cursor], " " * (end - cursor)))
            start = cursor = end
            continue
        cursor += 1
    chunks.append(text[start:])
    return "".join(chunks)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key {key!r}")
        result[key] = value
    return result


def finite_float(token):
    value = float(token)
    if not math.isfinite(value):
        raise ValueError(f"non-finite number {token!r}")
    return value


def validate_json(value, depth=0):
    if depth > 64:
        raise ValueError("config exceeds 64 levels of nesting")
    if isinstance(value, str):
        value.encode("utf-8")  # Reject unpaired surrogates accepted by json.loads.
    elif isinstance(value, dict):
        for key, item in value.items():
            key.encode("utf-8")
            validate_json(item, depth + 1)
    elif isinstance(value, list):
        for item in value:
            validate_json(item, depth + 1)


def load_legacy(path):
    # A FIFO supplied as input must not block an explicit migration run.
    with open(path, "rb", opener=open_nonblocking) as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError("legacy config must be a regular file")
        data = source.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError("legacy config exceeds 1 MiB")
    text = strip_line_comments(data.decode("utf-8"))
    document = json.loads(
        text, object_pairs_hook=unique_object, parse_constant=finite_float, parse_float=finite_float
    )
    if not isinstance(document, dict):
        raise ValueError("legacy config must be an object")
    validate_json(document)
    if "terminal" in document:
        raise ValueError("input already contains a terminal section, expected the legacy config")
    return document


def open_nonblocking(path, flags):
    return os.open(path, flags | os.O_NONBLOCK)


def number(value, name, minimum, maximum):
    if type(value) not in (int, float) or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be a number in [{minimum}, {maximum}]")
    return value


def integer(value, name, minimum):
    if type(value) is not int or not minimum <= value <= I32_MAX:
        raise ValueError(f"{name} must be an integer in [{minimum}, {I32_MAX}]")
    return value


def string(value, name):
    if not isinstance(value, str) or "\0" in value:
        raise ValueError(f"{name} must be a string without NUL bytes")
    return value


def boolean(value, name):
    if type(value) is not bool:
        raise ValueError(f"{name} must be a boolean")
    return value


def absolute_path(value, option):
    value = string(value, option)
    value.encode("utf-8")
    if not Path(value).is_absolute():
        raise ValueError(f"{option} requires an absolute path")
    return value


def crop_region(region):
    fields = ("x", "y", "width", "height")
    if not isinstance(region, dict) or region.keys() - set(fields):
        raise ValueError("model_region must be an object with x, y, width and height")
    # The legacy x/y default to zero. Retain the requested rectangle (including
    # negative origins) since both runtimes clip it against the current output.
    return {
        key: integer(region.get(key, 0), f"model_region.{key}", -(1 << 31) if key in ("x", "y") else 1)
        for key in fields
    }


def migrate(legacy, model_directory=None, startup_model=None):
    terminal = {}
    warnings = []
    for old, new in (("idle_opacity", "opacity"), ("mouse_look_radius", "mouse_look_radius")):
        if old in legacy:
            upper = 1 if old == "idle_opacity" else F32_MAX
            terminal[new] = number(legacy[old], old, 0, upper)
    if terminal.get("mouse_look_radius", 0) > 1:
        warnings.append("terminal caps the effective mouse_look_radius at 1")

    if "hover_opacity" in legacy or "hover_opacity_enabled" in legacy:
        hover = number(legacy.get("hover_opacity", 0.5), "hover_opacity", 0, 1)
        enabled = boolean(legacy.get("hover_opacity_enabled", False), "hover_opacity_enabled")
        terminal["hover_opacity"] = hover if enabled else None

    output = string(legacy.get("model_output", ""), "model_output")
    if output:
        if len(output.encode("utf-8")) > 256 or re.search(r"[\x00-\x1f\x7f-\x9f]", output):
            raise ValueError("model_output must be at most 256 UTF-8 bytes without control characters")
        terminal["output"] = output
    gpu = string(legacy.get("render_device", ""), "render_device")
    if gpu:
        if gpu != "auto" and not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\.[0-7]", gpu):
            raise ValueError("render_device must be auto or a lowercase PCI address such as 0000:00:02.0")
        terminal["gpu"] = gpu

    default_model = string(legacy.get("default_model", ""), "default_model")
    if startup_model is not None:
        startup_model = absolute_path(startup_model, "--startup-model")
        if not startup_model.endswith(".model3.json"):
            raise ValueError("--startup-model requires a .model3.json file path")
        terminal["startup_model"] = startup_model
    elif default_model:
        warnings.append("default_model is a legacy folder name. Supply --startup-model with its .model3.json path")

    roots = legacy.get("additional_model_dirs", [])
    if not isinstance(roots, list):
        raise ValueError("additional_model_dirs must be an array of strings")
    roots = [string(root, "additional_model_dirs entry") for root in roots]
    if model_directory is not None:
        terminal["model_directory"] = absolute_path(model_directory, "--model-directory")
    elif len(roots) == 1 and Path(roots[0]).is_absolute():
        terminal["model_directory"] = roots[0]
        warnings.append("using the sole additional_model_dirs root as model_directory. The previous primary directory is not inferred")
    elif roots:
        warnings.append("additional_model_dirs requires a choice of one absolute root via --model-directory")

    scale = number(legacy.get("model_scale", 1), "model_scale", 0, F32_MAX)
    if scale == 0:
        raise ValueError("model_scale must be positive")
    x = number(legacy.get("model_x", 0), "model_x", -F32_MAX, F32_MAX)
    y = number(legacy.get("model_y", 0), "model_y", -F32_MAX, F32_MAX)
    if (x, y, scale) != (0, 0, 1):
        # Legacy projection scales model-space translation, uses the actual
        # viewport aspect and has a portrait/model-canvas branch. Native Rust
        # fits the asset to a fixed base canvas and moves its logical top-left.
        warnings.append("model_x, model_y and model_scale require manual adjustment in terminal.transform because the render projections differ")
    if "model_region" in legacy:
        terminal["transform"] = {"x": 0.0, "y": 0.0, "zoom": 1.0, "crop": crop_region(legacy["model_region"])}
        warnings.append("model_region retains its output-local logical coordinates. Preview the crop with the new model placement")

    if "emotion_timeout" in legacy:
        timeout = number(legacy["emotion_timeout"], "emotion_timeout", -F32_MAX, 86_400)
        terminal["emotion_timeout"] = -1 if timeout < 0 else timeout

    unsupported = {
        "window_width": lambda value: integer(value, "window_width", 1),
        "window_height": lambda value: integer(value, "window_height", 1),
        "fullscreen_opacity_enabled": lambda value: boolean(value, "fullscreen_opacity_enabled"),
        "fullscreen_opacity": lambda value: number(value, "fullscreen_opacity", 0, 1),
        "plugin_dir": lambda value: string(value, "plugin_dir"),
    }
    skipped = []
    for key, validate in unsupported.items():
        if key in legacy:
            validate(legacy[key])
            skipped.append(key)
    if skipped:
        warnings.append("unsupported legacy settings: " + ", ".join(skipped))
    known = set(unsupported) | {
        "idle_opacity", "mouse_look_radius", "hover_opacity", "hover_opacity_enabled",
        "model_output", "render_device", "default_model", "additional_model_dirs",
        "model_scale", "model_x", "model_y", "model_region", "emotion_timeout",
    }
    if unknown := sorted(legacy.keys() - known):
        warnings.append("unrecognized legacy settings: " + ", ".join(map(repr, unknown)))
    return {"terminal": terminal}, warnings


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Review the proposal and merge its terminal keys into your existing config.jsonc. "
        "For a migrated crop, retain any existing transform position and zoom that you want to keep.",
    )
    parser.add_argument("legacy_config", type=Path, help="path to the legacy desktop config.json (read-only)")
    parser.add_argument("--model-directory", help="absolute model catalog root to use")
    parser.add_argument("--startup-model", help="absolute .model3.json path to use at startup")
    args = parser.parse_args()
    try:
        proposed, warnings = migrate(load_legacy(args.legacy_config), args.model_directory, args.startup_model)
        encoded = json.dumps(proposed, allow_nan=False, indent=2)
        if len(encoded) + 1 > MAX_BYTES:
            raise ValueError("proposed config exceeds 1 MiB")
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"{parser.prog}: {args.legacy_config}: {error}\n")
    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    print(encoded)


if __name__ == "__main__":
    main()
