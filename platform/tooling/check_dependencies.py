"""Reject implementation imports across ownership boundaries using compiler dependencies."""
from pathlib import Path
import shlex
import subprocess
import sys


def owner(path, root):
    try:
        relative = path.resolve().relative_to(root)
    except ValueError:
        return "external"
    parts = relative.parts
    if not parts or parts[0].startswith("build") or parts[0] in ("thirdParty", "CubismSdkForNative-5-r.5"):
        return "external"
    if parts and parts[0] == "sdk":
        return "sdk"
    if parts and parts[0] == "platform":
        return "platform"
    if len(parts) > 2 and parts[:2] == ("src", "plugins"):
        return "plugin:" + parts[2]
    if parts[0] in ("cmake", "scripts", "tests", "packaging", ".gitea") or parts == ("install.sh",):
        return "platform"
    if len(parts) > 1 and parts[:2] == ("research", "ocr"):
        return "plugin:perception"
    if parts == ("deskwaifu-ctl",):
        return "plugin:ipc"
    return "unassigned"


def main():
    root, expected, *command = sys.argv[1:]
    root = Path(root).resolve()
    result = subprocess.run(command)
    if result.returncode:
        return result.returncode
    if "-MF" not in command:
        print("Ownership check requires compiler dependency output (-MF)", file=sys.stderr)
        return 1
    dependencies = Path(command[command.index("-MF") + 1]).read_text()
    dependencies = dependencies.replace("\\\n", " ").split(":", 1)[1]
    for dependency in shlex.split(dependencies):
        actual = owner(Path(dependency), root)
        if actual not in (expected, "sdk", "external"):
            print(f"Ownership violation: {expected} imports {actual}: {dependency}", file=sys.stderr)
            if "-o" in command:
                Path(command[command.index("-o") + 1]).unlink(missing_ok=True)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
