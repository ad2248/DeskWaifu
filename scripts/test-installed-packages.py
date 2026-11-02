#!/usr/bin/env python3
"""Install/test four split packages in a disposable, dependency-only PRoot.

No source/build tree or private SDK is mounted into the installation. System
dependencies are copied from the same fixed image, never downloaded/upgraded.
PRoot needs no nested container socket, mount capability or privileged runner.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid

HERE = Path(__file__).resolve().parent
PACKAGES = ("deskwaifu-bin", "deskwaifu-ocr-bin", "deskwaifu-tests", "deskwaifu-ocr-tests")


def sections(text):
    result, key = {}, None
    for line in text.splitlines():
        if line.startswith("%") and line.endswith("%"):
            key = line.strip("%")
            result[key] = []
        elif line and key:
            result[key].append(line)
    return result


class Root:
    def __init__(self, root):
        self.root = root
        self.installed = set()
        self.packages, self.providers = {}, {}
        for directory in Path("/var/lib/pacman/local").iterdir():
            if not directory.is_dir():
                continue
            metadata = sections((directory / "desc").read_text())
            name = metadata["NAME"][0]
            self.packages[name] = (directory, metadata)
            for capability in [name, *metadata.get("PROVIDES", [])]:
                self.providers[re.split(r"[<>=]", capability)[0]] = name
        for name in ("tmp", "dev", "proc", "sys", "run", "var/lib/pacman/local", "var/cache/pacman/pkg"):
            (root / name).mkdir(parents=True, exist_ok=True)
        (root / "tmp").chmod(0o1777)
        shutil.copy2("/var/lib/pacman/local/ALPM_DB_VERSION", root / "var/lib/pacman/local/ALPM_DB_VERSION")

    def dependency(self, requirement):
        capability = re.split(r"[<>=]", requirement)[0]
        if capability in self.installed:
            return
        name = self.providers[capability]
        if name in self.installed:
            return
        directory, metadata = self.packages[name]
        self.installed.add(name)
        for dependency in metadata.get("DEPENDS", []):
            self.dependency(dependency)
        for relative in sections((directory / "files").read_text()).get("FILES", []):
            source = Path("/") / relative.rstrip("/")
            destination = self.root / relative.rstrip("/")
            if source.is_symlink():
                destination.parent.mkdir(parents=True, exist_ok=True)
                if not destination.is_symlink():
                    destination.symlink_to(os.readlink(source))
            elif source.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                destination.chmod(source.stat().st_mode & 0o7777)
            elif source.is_file():
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
        shutil.copytree(directory, self.root / "var/lib/pacman/local" / directory.name, symlinks=True)

    def install(self, package):
        text = subprocess.check_output(["bsdtar", "-xOf", str(package), ".PKGINFO"], text=True)
        for line in text.splitlines():
            if line.startswith("depend = "):
                self.dependency(line.removeprefix("depend = "))
        # pacman validates exact runtime/test package versions using the copied
        # installed database. Hook/script execution is unnecessary in this root.
        config = self.root / "pacman-test.conf"
        config.write_text("[options]\nArchitecture = x86_64\nSigLevel = Never\nHookDir = /no-hooks\n")
        subprocess.run(["pacman", "--root", str(self.root), "--config", str(config), "--dbpath",
                        str(self.root / "var/lib/pacman"), "--noscriptlet", "--noconfirm", "-U", str(package)], check=True)
        name = next(line.split(" = ", 1)[1] for line in text.splitlines() if line.startswith("pkgname = "))
        self.installed.add(name)

    def run(self, command):
        marker = "/tmp/exit-" + uuid.uuid4().hex
        # A traced D-Bus daemon may outlive its session leader and PRoot can
        # report the daemon's status instead. Require the actual command's
        # recorded result; missing markers and nonzero results fail closed.
        launcher = ("import pathlib,subprocess,sys; "
                    "r=subprocess.run(sys.argv[2:]); "
                    "pathlib.Path(sys.argv[1]).write_text(str(r.returncode)); sys.exit(r.returncode)")
        subprocess.run(["runuser", "-u", "nobody", "--", "env", "-i", "PATH=/usr/bin:/bin", "LANG=C.UTF-8",
                        "OPENCV_FOR_THREADS_NUM=2", "/usr/local/bin/proot", "-r", str(self.root),
                        "-b", "/dev", "-b", "/proc", "-b", "/sys", "-w", "/tmp",
                        "python3", "-c", launcher, marker, *command], check=True, timeout=900)
        status = self.root / marker.lstrip("/")
        if not status.is_file() or status.read_text() != "0":
            raise RuntimeError(f"Installed command failed or did not finish: {command}")
        status.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", type=Path)
    parser.add_argument("version")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="deskwaifu-installed-", dir="/tmp") as temporary:
        root = Path(temporary)
        root.chmod(0o755)
        guest = Root(root)
        guest.dependency("filesystem")
        guest.dependency("bash")
        guest.dependency("python")
        guest.dependency("dbus")
        (root / "etc/machine-id").write_text("00000000000000000000000000000001\n")
        smoke = root / "smoke"
        smoke.mkdir()
        shutil.copy2(HERE / "package-smoke.py", smoke)
        shutil.copy2(HERE.parent / "tests/test_ocr_native_models.py", smoke)
        for index, name in enumerate(PACKAGES):
            print(f"=== Stage {index + 1}/4: {name} ===", flush=True)
            guest.install(args.packages.resolve() / f"{name}-{args.version}-1-x86_64.pkg.tar.zst")
            if index == 0:
                assert "opencv" not in guest.installed and "yaml-cpp" not in guest.installed
                assert not (root / "usr/lib/deskwaifu/ocr/deskwaifu-ocr.so").exists()
            if index < 2:
                guest.run(["dbus-run-session", "--", "python3", "/smoke/package-smoke.py", "ocr" if index else "platform"])
            else:
                suite = "ocr-tests" if index == 3 else "tests"
                prefix = f"/usr/lib/deskwaifu/{suite}"
                guest.run(["python3", f"{prefix}/test-environment.py", "python3", f"{prefix}/run.py"])
        print("All four installed-package stages passed; temporary root will be removed.", flush=True)


if __name__ == "__main__":
    main()
