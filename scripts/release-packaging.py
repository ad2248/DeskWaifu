#!/usr/bin/env python3
"""Render makepkg input and verify the single Rust package before upload."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent.parent
MAIN_DEPENDS = {"glibc", "gcc-libs"}
PACKAGE_NAMES = ("deskwaifu-bin",)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(directory, tag, commit):
    template = (ROOT / "packaging/release/PKGBUILD.in").read_text()
    # Substitute the untrusted tag last so tag text cannot become a placeholder.
    template = template.replace("@SOURCE_SHA@", digest(directory / "deskwaifu.tar.gz"))
    template = template.replace("@PKGVER@", shlex.quote(tag))
    (directory / "PKGBUILD").write_text(template)


def metadata(text):
    result = {}
    for line in text.splitlines():
        if " = " in line:
            key, value = line.split(" = ", 1)
            result.setdefault(key, []).append(value)
    return result


def verify_unversioned_dependencies(dependencies):
    if any(dep.startswith("deskwaifu") and any(operator in dep for operator in "<>=")
           for dep in dependencies):
        raise ValueError("DeskWaifu dependencies must use package names without a version constraint")


def verify_content(name, info, files, version):
    verify_unversioned_dependencies(info.get("depend", []))
    if (name not in PACKAGE_NAMES or info.get("pkgname") != [name] or
            info.get("pkgver") != [version + "-1"] or info.get("arch") != ["x86_64"]):
        raise ValueError(f"Wrong package identity/version/architecture: {name}")
    if set(info.get("depend", [])) != MAIN_DEPENDS:
        raise ValueError("Unexpected Rust runtime dependencies")
    # Only makepkg metadata, directory entries and the application are allowed.
    # This rejects old plugins, SDKs, OCR assets and installed-test payloads.
    allowed = {".PKGINFO", ".BUILDINFO", ".MTREE", "usr/", "usr/bin/", "usr/bin/deskwaifu"}
    files = {path.removeprefix("./") for path in files}
    if not files <= allowed:
        raise ValueError(f"Unexpected package files: {sorted(files - allowed)}")
    if "usr/bin/deskwaifu" not in files:
        raise ValueError("Missing package files: usr/bin/deskwaifu")


def verify(directory, tag, commit):
    packages = sorted(directory.glob("*.pkg.tar.zst"))
    expected = {f"deskwaifu-bin-{tag}-1-x86_64.pkg.tar.zst": "deskwaifu-bin"}
    if {path.name for path in packages} != expected.keys():
        raise ValueError("Unexpected or missing versioned x86_64 packages")
    for path in packages:
        info = metadata(subprocess.check_output(["bsdtar", "-xOf", str(path), ".PKGINFO"], text=True))
        files = subprocess.check_output(["bsdtar", "-tf", str(path)], text=True).splitlines()
        verify_content(expected[path.name], info, files, tag)
    manifest = directory / "release-metadata.json"
    manifest.write_text(json.dumps({"tag": tag, "commit": commit, "pkgver": tag, "pkgrel": 1},
                                   sort_keys=True, indent=2) + "\n")
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
