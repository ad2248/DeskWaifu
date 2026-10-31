#!/usr/bin/env python3
"""Verify a public GitHub release and update the clean AUR submodule (no push)."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import quote
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "ad2248/DeskWaifu"
PACKAGES = ("deskwaifu-bin", "deskwaifu-ocr-bin")


def release_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def download(url):
    with urlopen(url, timeout=120) as response:
        if not response.url.startswith("https://"):
            raise ValueError("Release downloads must use HTTPS")
        return response.read()


def public_tag_commit(tag):
    api = f"https://api.github.com/repos/{REPOSITORY}/git/"
    obj = json.loads(download(api + "ref/tags/" + quote(tag, safe="")))["object"]
    for _ in range(10):
        if obj["type"] == "commit":
            return obj["sha"]
        if obj["type"] != "tag":
            break
        obj = json.loads(download(api + "tags/" + obj["sha"]))["object"]
    raise ValueError("GitHub tag does not resolve to a commit")


def require_clean_submodule(recipe):
    if not (recipe / ".git").is_file():
        raise ValueError("Initialize packaging/aur as a submodule before updating")
    parent = subprocess.check_output(
        ["git", "rev-parse", "--show-superproject-working-tree"], cwd=recipe, text=True).strip()
    if Path(parent).resolve() != ROOT:
        raise ValueError("packaging/aur is not this repository's submodule")
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=recipe, text=True):
        raise ValueError("AUR submodule has local changes; commit or save them first")


def verify_assets(directory, tag, commit):
    # Reuse the producer's manifest, checksum and payload contract.
    release_tool("release-upload").load_assets(directory, tag, commit)
    packaging = release_tool("release-packaging")
    checksums = []
    for name in PACKAGES:
        path = directory / f"{name}-{tag}-1-x86_64.pkg.tar.zst"
        info = packaging.metadata(subprocess.check_output(
            ["bsdtar", "-xOf", str(path), ".PKGINFO"], text=True))
        files = subprocess.check_output(["bsdtar", "-tf", str(path)], text=True).splitlines()
        # The consumer only extracts usr/. Reject payloads it would silently lose.
        for entry in files:
            if entry in {".PKGINFO", ".BUILDINFO", ".MTREE"}:
                continue
            if not entry.startswith("usr/") or ".." in Path(entry).parts:
                raise ValueError(f"Unsupported binary package path: {entry}")
        packaging.verify_content(name, info, files, tag)
        checksums.append(hashlib.sha256(path.read_bytes()).hexdigest())
    return checksums


def render_recipe(text, tag, checksums):
    old_tag = re.search(r"^_tag=(\S+)$", text, re.M).group(1)
    old_sums = re.search(r"^sha256sums=\(\n(.*?)\n\)", text, re.M | re.S)
    sums = "\n".join(f"  '{value}'" for value in checksums)
    if old_tag == tag and old_sums.group(1) != sums:
        raise ValueError("Assets changed for an existing release; refusing replacement")
    for key, value in {"pkgver": tag, "_tag": tag, "_upstream_pkgrel": "1"}.items():
        text, count = re.subn(rf"^{key}=.*$", f"{key}={value}", text, flags=re.M)
        if count != 1:
            raise ValueError(f"Expected one {key} assignment")
    if tag != old_tag:
        text = re.sub(r"^pkgrel=.*$", "pkgrel=1", text, flags=re.M)
    return re.sub(r"^sha256sums=\(\n.*?\n\)",
                  f"sha256sums=(\n{sums}\n)", text, flags=re.M | re.S)


def update(tag):
    # Current producer uses the tag unchanged as pkgver and fixes upstream pkgrel=1.
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+_]*", tag):
        raise ValueError("Tag must be an Arch pkgver (no slash, whitespace, colon or hyphen)")
    recipe = ROOT / "packaging/aur"
    require_clean_submodule(recipe)
    with tempfile.TemporaryDirectory(prefix="deskwaifu-aur-") as temporary:
        directory = Path(temporary)
        base = f"https://github.com/{REPOSITORY}/releases/download/{quote(tag, safe='')}/"
        names = [f"{name}-{tag}-1-x86_64.pkg.tar.zst" for name in PACKAGES]
        names += ["release-metadata.json", "SHA256SUMS"]
        for name in names:
            (directory / name).write_bytes(download(base + name))
        commit = public_tag_commit(tag)
        checksums = verify_assets(directory, tag, commit)
        text = render_recipe((recipe / "PKGBUILD").read_text(), tag, checksums)
        # Generate metadata before changing either maintained file.
        (directory / "PKGBUILD").write_text(text)
        srcinfo = subprocess.check_output(["makepkg", "--printsrcinfo"], cwd=directory, text=True)
        (recipe / "PKGBUILD").write_text(text)
        (recipe / ".SRCINFO").write_text(srcinfo)
    subprocess.run(["git", "diff", "--", "PKGBUILD", ".SRCINFO"], cwd=recipe, check=True)
    print("Verified release. Review and test before committing/pushing AUR and its parent pointer.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="Exact public release tag; never latest or a branch")
    args = parser.parse_args()
    try:
        update(args.tag)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
