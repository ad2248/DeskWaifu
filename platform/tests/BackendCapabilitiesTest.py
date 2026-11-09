"""Real platform/plugin composition without a desktop, cloud or audio device."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def main():
    executable, *modules = sys.argv[1:]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        for module in modules:
            source = Path(module)
            destination = root / source.stem / source.name
            destination.parent.mkdir()
            shutil.copyfile(source, destination)
        process = subprocess.Popen([executable], env=dict(os.environ, DESKWAIFU_PLUGIN_DIR=str(root), XDG_RUNTIME_DIR=str(runtime)),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            path = runtime / "deskwaifu.sock"
            deadline = time.monotonic() + 5
            while not path.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(.02)
            assert path.exists(), "IPC plugin did not publish its socket"
            expected = {"get_status": False, "unknown_command": False}
            names = {Path(module).stem for module in modules}
            expected["get_ocr_status"] = "perception" in names
            expected["get_speech_settings"] = "speech" in names
            expected["get_reading_clip_status"] = "reading_clip" in names
            for command, available in expected.items():
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(3)
                    client.connect(str(path))
                    client.sendall((json.dumps({"command": command}) + "\n").encode())
                    with client.makefile() as stream:
                        reply = json.loads(stream.readline())
                assert reply["ok"] is available, (command, reply)
                if not available:
                    assert reply["error"] == "no provider", (command, reply)
            assert process.poll() is None, "A missing optional capability terminated the platform"
        finally:
            process.terminate()
            try:
                output, error = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                output, error = process.communicate()
                raise
            sys.stderr.write(error.decode(errors="replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
