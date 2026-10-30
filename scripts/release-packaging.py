#!/usr/bin/env python3
"""Render makepkg input and verify the actual split-package payload before upload."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("release_source", ROOT / "scripts/release-source.py")
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)
PLUGINS = ("desktop", "perception", "ipc", "speech", "reading_clip", "system", "interaction", "model", "log")
MAIN_DEPENDS = {"glibc", "gcc-libs", "wayland", "libglvnd", "systemd-libs", "qt6-base>=6.5",
                "qt6-wayland>=6.5", "qt6-scxml>=6.5", "qt6-websockets>=6.5", "qt6-multimedia>=6.5",
                "layer-shell-qt>=6.6", "bash", "python", "psmisc"}
SDK_LIBRARIES = {f"usr/lib/libdeskwaifu_{name}.so.{version}" for name in ("events", "plugin_sdk", "gui_plugin_sdk")
                 for version in ("2", "2.0")}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(directory, tag, commit):
    version = source.package_version(tag)
    template = (ROOT / "packaging/release/PKGBUILD.in").read_text()
    for key, value in {"PKGVER": version, "SOURCE_SHA": digest(directory / "deskwaifu.tar.gz"),
                       "PRIVATE_SHA": digest(directory / "private-deps.tar.gz")}.items():
        template = template.replace(f"@{key}@", value)
    (directory / "PKGBUILD").write_text(template)


def metadata(text):
    result = {}
    for line in text.splitlines():
        if " = " in line:
            key, value = line.split(" = ", 1)
            result.setdefault(key, []).append(value)
    return result


def verify_content(name, info, files, version):
    if info.get("pkgname") != [name] or info.get("pkgver") != [version + "-1"] or info.get("arch") != ["x86_64"]:
        raise ValueError(f"Wrong package identity/version/architecture: {name}")
    files = {path.removeprefix("./").rstrip("/") for path in files}
    if any(path.endswith((".onnx", ".pdiparams", ".pdmodel")) or
           path.startswith("usr/share/deskwaifu/ocr/") or
           "dictionary" in path.lower() or Path(path).name in {"inference.yml", "inference.yaml"} for path in files):
        raise ValueError("Models/dictionaries must not be shipped in release packages")
    if name == "deskwaifu-bin":
        required = {"usr/bin/deskwaifu", "usr/bin/deskwaifu-control", "usr/bin/deskwaifu-ctl", "usr/bin/deskwaifu-cursor-guard"}
        required.update(f"usr/lib/deskwaifu/plugins/{plugin}/{plugin}.so" for plugin in PLUGINS)
        required.update(SDK_LIBRARIES)
        if any(path.startswith("usr/lib/deskwaifu/ocr/") or path == "usr/libexec/deskwaifu-ocr" for path in files):
            raise ValueError("Main package must not contain OCR helper/runtime")
        if any(dep.startswith(("opencv", "onnxruntime", "deskwaifu-ocr")) for dep in info.get("depend", [])):
            raise ValueError("Main package must not require the optional OCR runtime")
        if not MAIN_DEPENDS <= set(info.get("depend", [])):
            raise ValueError(f"Missing main runtime dependencies: {sorted(MAIN_DEPENDS - set(info.get('depend', [])))}")
    else:
        required = {"usr/libexec/deskwaifu-ocr", "usr/lib/deskwaifu/ocr/libonnxruntime.so.1"}
        if not {f"deskwaifu-bin={version}-1", "opencv", "yaml-cpp"} <= set(info.get("depend", [])):
            raise ValueError("OCR package needs the exact main package version, OpenCV and yaml-cpp")
        if any(path.startswith("usr/lib/deskwaifu/plugins/") for path in files):
            raise ValueError("OCR package must not duplicate main plugins")
    if not required <= files:
        raise ValueError(f"Missing package files: {sorted(required - files)}")


def verify(directory, tag, commit):
    version = source.package_version(tag)
    packages = sorted(directory.glob("*.pkg.tar.zst"))
    expected = {f"{name}-{version}-1-x86_64.pkg.tar.zst": name for name in ("deskwaifu-bin", "deskwaifu-ocr-bin")}
    if {path.name for path in packages} != expected.keys():
        raise ValueError("Expected exactly the two versioned x86_64 release packages")
    for path in packages:
        info = metadata(subprocess.check_output(["bsdtar", "-xOf", str(path), ".PKGINFO"], text=True))
        files = subprocess.check_output(["bsdtar", "-tf", str(path)], text=True).splitlines()
        verify_content(expected[path.name], info, files, version)
    manifest = directory / "release-metadata.json"
    manifest.write_text(json.dumps({"tag": tag, "commit": commit, "pkgver": version, "pkgrel": 1,
                                    "ocr_models_included": False}, sort_keys=True, indent=2) + "\n")
    assets = sorted([*packages, manifest])
    (directory / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.name}\n" for path in assets))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "verify"))
    parser.add_argument("directory", type=Path)
    parser.add_argument("tag")
    parser.add_argument("commit")
    args = parser.parse_args()
    {"prepare": prepare, "verify": verify}[args.command](args.directory, args.tag, args.commit)
