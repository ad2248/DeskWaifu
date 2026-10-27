"""Check the resolved project's artifacts before invoking the real linker."""
from pathlib import Path
import shlex
import subprocess
import sys


def arguments(tokens, seen=None):
    """Expose linker forwarding and response files without changing the command."""
    seen = set() if seen is None else seen
    for token in tokens:
        if token.startswith("-Wl,"):
            yield from arguments(token[4:].split(","), seen)
        elif token.startswith("@"):
            path = Path(token[1:]).resolve()
            if path not in seen:
                seen.add(path)
                yield from arguments(shlex.split(path.read_text()), seen)
        else:
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
    tokens = list(arguments(command))
    search = []
    libraries = []
    for index, token in enumerate(tokens):
        if token in ("-L", "-l") and index + 1 < len(tokens):
            value = tokens[index + 1]
        else:
            value = token[2:]
        if token.startswith("-L"):
            search.append(Path(value).resolve())
        elif token.startswith("-l"):
            libraries.append(value)
    paths = [Path(token).resolve() for token in tokens if not token.startswith("-")]
    for library in libraries:
        names = [library[1:]] if library.startswith(":") else ["lib" + library + suffix for suffix in (".so", ".a")]
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
