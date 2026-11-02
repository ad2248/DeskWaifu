#!/usr/bin/env python3
"""External smoke harness: only installed runtime packages are exercised."""
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import tempfile
import time


def request(path, message):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(5)
        client.connect(str(path))
        client.sendall((json.dumps(message) + "\n").encode())
        with client.makefile() as stream:
            return json.loads(stream.readline())


def main():
    ocr = sys.argv[1] == "ocr"
    with tempfile.TemporaryDirectory(prefix="package-smoke-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        plugins = root / "plugins"
        plugins.mkdir()
        for name in ("ipc", "perception"):
            directory = plugins / name
            directory.mkdir()
            shutil.copy2(Path("/usr/lib/deskwaifu/plugins") / name / f"{name}.so", directory / f"{name}.so")
        config = root / "config.json"
        values = {"plugin_dir": str(plugins)}
        if ocr:
            # Reuse the stdlib-only tiny synthetic ONNX generators, not real
            # model downloads. No test package is installed during this stage.
            from test_ocr_native_models import NativeModelDirectories
            fixture = NativeModelDirectories()
            fixture.setUp()
            values.update(ocr_detection_model_dir=str(fixture.detection),
                          ocr_recognition_model_dir=str(fixture.recognition))
            image = root / "image.ppm"
            image.write_bytes(b"P6\n64 64\n255\n" + b"\xff" * (64 * 64 * 3))
        config.write_text(json.dumps(values))
        plugin_config = root / "plugin/perception/config.json"
        plugin_config.parent.mkdir(parents=True)
        plugin_config.write_text(json.dumps(values))
        env = dict(os.environ, HOME=str(root), XDG_RUNTIME_DIR=str(runtime), QT_QPA_PLATFORM="offscreen")
        with (root / "backend.log").open("w+") as log:
            process = subprocess.Popen(["/usr/bin/deskwaifu", "--config", str(config)], env=env, stdout=log, stderr=log)
            try:
                path = runtime / "deskwaifu.sock"
                deadline = time.monotonic() + 30
                while not path.exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(.05)
                assert path.exists(), "Installed backend failed to publish IPC socket"
                while True:
                    status = request(path, {"command": "get_ocr_status"})
                    if status.get("availability_state") != "checking":
                        break
                    assert time.monotonic() < deadline, status
                    time.sleep(.05)
                if ocr:
                    assert status["available"], status
                    reply = request(path, {"command": "ocr_recognize", "image": str(image)})
                    assert reply["ok"], reply
                    while True:
                        result = request(path, {"command": "get_ocr_result", "job_id": reply["job_id"]})
                        if result.get("state") != "pending":
                            break
                        assert time.monotonic() < deadline, result
                        time.sleep(.05)
                    assert result["ok"] and result["response"]["ok"] and result["response"]["result"]["lines"][0]["text"] == "中' ", result
                else:
                    assert status["error_code"] == "module_missing", status
                assert process.poll() is None
                print("Installed " + ("OCR inference" if ocr else "platform IPC / optional OCR absence") + " smoke passed")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.seek(0)
                print(log.read())
                if ocr:
                    fixture.doCleanups()


if __name__ == "__main__":
    main()
