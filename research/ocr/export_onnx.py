"""Build the local PP-OCRv6 small_det + medium_rec bundle (build-time Python only)."""

import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import subprocess
import sys

import onnx
import yaml


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("output/onnx-models"))
    args = parser.parse_args()
    args.models_root = args.models_root.expanduser().resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = {}
    for role, model in (("detection", "PP-OCRv6_small_det"), ("recognition", "PP-OCRv6_medium_rec")):
        directory = args.models_root.expanduser().resolve() / model
        for filename in ("inference.json", "inference.pdiparams", "inference.yml"):
            path = directory / filename
            sources[f"{model}/{filename}"] = sha256(path)
        target = args.output / f"{role}.onnx"
        subprocess.run([
            sys.executable, "-m", "paddle2onnx.command",
            "--model_dir", str(directory), "--model_filename", "inference.json",
            "--params_filename", "inference.pdiparams", "--save_file", str(target),
            "--opset_version", "19", "--optimize_tool", "None",
        ], check=True)
        onnx.checker.check_model(str(target))
    config = yaml.safe_load((args.models_root / "PP-OCRv6_medium_rec/inference.yml").read_text())
    characters = [""] + config["PostProcess"]["character_dict"] + [" "]
    (args.output / "dictionary.json").write_text(json.dumps(characters, ensure_ascii=False) + "\n")
    manifest = {
        "profile": "ppocrv6-small-medium-v1",
        "models": ["PP-OCRv6_small_det", "PP-OCRv6_medium_rec"],
        "source_sha256": sources,
        "tools": {name: importlib.metadata.version(name) for name in ("paddlepaddle", "paddle2onnx", "onnx")},
        "sha256": {name: sha256(args.output / name) for name in ("detection.onnx", "recognition.onnx", "dictionary.json")},
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
