"""Check the resolved project's artifacts before invoking the real linker."""
from pathlib import Path
import shlex
import subprocess
import sys


def arguments(tokens, seen=None, forwarded=False):
    """Expose linker forwarding and response files without changing the command."""
    seen = set() if seen is None else seen
    tokens = iter(tokens)
    for token in tokens:
        linker = forwarded
        if token == "-Xlinker":
            token = next(tokens)
            linker = True
        if token.startswith("-Wl,"):
            yield from arguments(token[4:].split(","), seen, True)
        elif token.startswith("@"):
            path = Path(token[1:]).resolve()
            if path in seen:
                sys.exit(f"Cyclic linker response file: {path}")
            yield from arguments(shlex.split(path.read_text()), seen | {path}, linker)
        else:
            yield token, linker


def options(tokens):
    """Canonicalize options whose operands may be separate linker arguments."""
    tokens = iter(tokens)
    for token in tokens:
        for short, long in (("-L", "--library-path"), ("-l", "--library")):
            if token in (short, long):
                token = short + next(tokens)
                break
            if token.startswith(long + "="):
                token = short + token.split("=", 1)[1]
                break
        yield token


def main():
    build, expected, *command = sys.argv[1:]
    manifests = list(Path(build).glob("deskwaifu-artifacts-*.txt"))
    if not manifests:
        sys.exit("Ownership check requires the generated artifact manifest")
    files, directories = {}, {}
    for manifest in manifests:
        for line in manifest.read_text().splitlines():
            kind, owner, path = line.split("\t", 2)
            (files if kind == "file" else directories)[Path(path).resolve()] = owner
    expanded = list(arguments(command))
    driver_static = ("-static", "--static", "-static-pie")
    static = any(token in driver_static and not linker for token, linker in expanded)
    # Driver-wide flags apply regardless of position. Forwarded linker aliases
    # are positional and must participate in --push-state/--pop-state instead.
    tokens = list(options(token for token, linker in expanded if linker or token not in driver_static))
    search = [Path(token[2:]).resolve() for token in tokens if token.startswith("-L")]
    paths = [Path(token).resolve() for token in tokens if not token.startswith("-")]
    modes = []
    for token in tokens:
        if token in ("-static", "--static", "-Bstatic", "-dn", "-non_shared", "-aarchive"):
            static = True
        elif token in ("-Bdynamic", "-dy", "-call_shared", "-ashared", "-adefault"):
            static = False
        elif token == "--push-state":
            modes.append(static)
        elif token == "--pop-state":
            static = modes.pop()
        if not token.startswith("-l"):
            continue
        library = token[2:]
        names = [library[1:]] if library.startswith(":") else ["lib" + library + suffix for suffix in ((".a",) if static else (".so", ".a"))]
        for directory in search:
            found = next((directory / name for name in names if (directory / name).exists()), None)
            if found:
                paths.append(found.resolve())
                break
    for path in paths:
        actual = files.get(path)
        if actual is None:
            actual = next((owner for directory, owner in directories.items() if path.is_relative_to(directory)), None)
        if actual is not None and actual not in (expected, "sdk", "external"):
            sys.exit(f"Ownership violation: {expected} cannot link {actual}: {path}")
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main())
