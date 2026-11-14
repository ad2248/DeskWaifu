"""Generate a private comma-decimal locale; never modify system locales."""
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    localedef = shutil.which("localedef")
    if not localedef:
        print("SKIP: localedef is unavailable")
        return 77
    binary, dbus_run_session, build_dir = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="kwin-locale-", dir=build_dir) as directory:
        result = subprocess.run(
            [localedef, "--no-archive", "-i", "de_DE", "-f", "UTF-8",
             os.path.join(directory, "de_DE.UTF-8")],
            capture_output=True, text=True,
        )
        if result.returncode:
            print("SKIP: cannot generate de_DE.UTF-8:", result.stderr)
            return 77
        env = dict(os.environ, LOCPATH=directory)
        return subprocess.run(
            [dbus_run_session, "--", binary, "--numeric-locale", "de_DE.UTF-8"],
            env=env,
        ).returncode


if __name__ == "__main__":
    sys.exit(main())
