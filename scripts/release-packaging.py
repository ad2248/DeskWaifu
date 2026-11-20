#!/usr/bin/env python3
"""Render makepkg input and verify the single Rust package before upload."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tarfile
import tomllib

ROOT = Path(__file__).resolve().parent.parent
MAIN_DEPENDS = {"glibc", "gcc-libs", "libpipewire", "wl-clipboard"}
PACKAGE_NAMES = ("deskwaifu-bin",)
TEST_PLUGINS = ("libdeskwaifu_test_plugin.so",)
PLUGINS_BY_PAYLOAD = {
    "rust-v1": TEST_PLUGINS,
    "rust-v2": (*TEST_PLUGINS, "libdeskwaifu_voice_input.so"),
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def select_payload(manifest):
    # Use declarations in the selected source, never the presence of build outputs.
    members = tomllib.loads(manifest.decode()).get("workspace", {}).get("members", [])
    if "plugins/test-plugin" not in members:
        raise ValueError("Selected source lacks the supported DeskWaifu Rust workspace")
    return "rust-v2" if "plugins/voice-input" in members else "rust-v1"


def prepare(directory, tag, commit):
    source = directory / "deskwaifu.tar.gz"
    with tarfile.open(source, "r|gz") as archive:
        for member in archive:
            if member.name == "deskwaifu/Cargo.toml" and member.isfile():
                with archive.extractfile(member) as manifest:
                    payload = select_payload(manifest.read())
                break
        else:
            raise ValueError("Selected source archive lacks deskwaifu/Cargo.toml")
    template = (ROOT / "packaging/release/PKGBUILD.in").read_text()
    # Substitute the untrusted tag last so tag text cannot become a placeholder.
    template = template.replace("@SOURCE_SHA@", digest(source))
    template = template.replace("@PLUGINS@", shlex.join(PLUGINS_BY_PAYLOAD[payload]))
    template = template.replace("@PKGVER@", shlex.quote(tag))
    (directory / "PKGBUILD").write_text(template)
    return payload


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


def verify_content(name, info, files, version, payload):
    verify_unversioned_dependencies(info.get("depend", []))
    if (name not in PACKAGE_NAMES or info.get("pkgname") != [name] or
            info.get("pkgver") != [version + "-1"] or info.get("arch") != ["x86_64"]):
        raise ValueError(f"Wrong package identity/version/architecture: {name}")
    if set(info.get("depend", [])) != MAIN_DEPENDS:
        raise ValueError("Unexpected Rust runtime dependencies")
    required = {"usr/bin/deskwaifu", "usr/bin/deskwaifucli", "usr/lib/deskwaifu/deskwaifu"}
    required.update(f"usr/lib/deskwaifu/plugins/{plugin}" for plugin in PLUGINS_BY_PAYLOAD[payload])
    allowed = required | {".PKGINFO", ".BUILDINFO", ".MTREE", "usr/", "usr/bin/",
                          "usr/lib/", "usr/lib/deskwaifu/", "usr/lib/deskwaifu/plugins/"}
    files = {path.removeprefix("./") for path in files}
    if not files <= allowed:
        raise ValueError(f"Unexpected package files: {sorted(files - allowed)}")
    if not required <= files:
        raise ValueError(f"Missing package files: {sorted(required - files)}")


def verify(directory, tag, commit, payload):
    packages = sorted(directory.glob("*.pkg.tar.zst"))
    expected = {f"deskwaifu-bin-{tag}-1-x86_64.pkg.tar.zst": "deskwaifu-bin"}
    if {path.name for path in packages} != expected.keys():
        raise ValueError("Unexpected or missing versioned x86_64 packages")
    for path in packages:
        info = metadata(subprocess.check_output(["bsdtar", "-xOf", str(path), ".PKGINFO"], text=True))
        files = subprocess.check_output(["bsdtar", "-tf", str(path)], text=True).splitlines()
        verify_content(expected[path.name], info, files, tag, payload)
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
    parser.add_argument("--payload", choices=PLUGINS_BY_PAYLOAD)
    args = parser.parse_args()
    if args.command == "prepare" and args.payload is None:
        print(prepare(args.directory, args.tag, args.commit))
    elif args.command == "verify" and args.payload is not None:
        verify(args.directory, args.tag, args.commit, args.payload)
    else:
        parser.error("--payload is required for verify and not accepted by prepare")
