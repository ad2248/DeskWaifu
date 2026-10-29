"""Manual-only OCR IPC smoke test in a private headless compositor (stdlib only)."""

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


def wait_for(predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    raise TimeoutError("smoke-test condition timed out")


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True, help="installed/staged helper with bundled models")
    parser.add_argument("--live2d-models", type=Path, required=True)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--compositor", choices=("sway", "kwin"), default="sway")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    args.logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ocr-smoke-", dir="/tmp/opencode") as temporary:
        runtime = Path(temporary)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(runtime / "config"),
                   DBUS_SESSION_BUS_ADDRESS=f"unix:path={runtime}/no-session-bus",
                   WLR_BACKENDS="headless", WLR_LIBINPUT_NO_DEVICES="1", WLR_RENDERER="pixman",
                   LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER="llvmpipe", QT_QPA_PLATFORM="wayland")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("SWAYSOCK", None)
        env.pop("QT_WAYLAND_SHELL_INTEGRATION", None)
        compositor_env = dict(env)
        compositor_command = ["sway", "--config", str(root / "containers/sway.config")]
        if args.compositor == "kwin":
            compositor_env["QT_QPA_PLATFORM"] = "offscreen"
            compositor_command = ["dbus-run-session", "--", "kwin_wayland", "--virtual", "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities"]
        with (args.logs / "compositor.log").open("w") as log:
            sway = subprocess.Popen(compositor_command, env=compositor_env, stdout=log, stderr=log, start_new_session=True)
        try:
            display = wait_for(lambda: next((p for p in runtime.glob("wayland-*") if p.is_socket()), None))
            env["WAYLAND_DISPLAY"] = display.name
            for installed in (False, True):
                env["DESKWAIFU_OCR_HELPER"] = str(args.helper.resolve()) if installed else str(runtime / "missing-helper")
                with (args.logs / f"app-{installed}.log").open("w") as log:
                    app = subprocess.Popen([str(args.app.resolve()), "--models_dir", str(args.live2d_models.resolve())], env=env, stdout=log, stderr=log)
                try:
                    endpoint = runtime / "deskwaifu.sock"
                    wait_for(endpoint.is_socket)

                    def command(name, **kwargs):
                        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                            connection.settimeout(3)
                            connection.connect(str(endpoint))
                            connection.sendall((json.dumps({"command": name, **kwargs}) + "\n").encode())
                            with connection.makefile("rb") as stream:
                                return json.loads(stream.readline())

                    def finished(job):
                        reply = command("get_ocr_result", job_id=job)
                        return reply if reply.get("state") != "pending" else None

                    def submit(sample):
                        reply = command("ocr_recognize", image=str((args.samples / sample).resolve()))
                        assert reply["ok"], reply
                        return reply["job_id"]

                    status = command("get_ocr_status")
                    assert status["available"] == installed and not status["running"], status
                    assert command("get_status")["ok"]
                    reading = command("get_reading_clip_status")
                    assert reading["ok"] and reading["state"] == "idle", reading
                    if not installed:
                        assert not command("ocr_recognize", image=str((args.samples / "light.png").resolve()))["ok"]
                    else:
                        first = submit("light.png")
                        assert not command("ocr_recognize", image=str((args.samples / "light.png").resolve()))["ok"]
                        start = time.monotonic()
                        assert command("get_status")["ok"]
                        responsive_s = time.monotonic() - start
                        result = wait_for(lambda: finished(first), timeout=60)
                        assert result["state"] == "completed", result
                        assert len(result["response"]["result"]["lines"]) == 3, result
                        second = submit("blank.png")
                        result = wait_for(lambda: finished(second))
                        assert result["state"] == "completed" and not result["response"]["result"]["lines"], result
                        cancelled = submit("dense.png")
                        assert command("cancel_ocr", job_id=cancelled)["ok"]
                        assert finished(cancelled)["state"] == "failed"
                        wait_for(lambda: not command("get_ocr_status")["running"])
                        crashed = submit("dense.png")
                        children_file = Path(f"/proc/{app.pid}/task/{app.pid}/children")

                        def worker_pid():
                            for value in children_file.read_text().split():
                                executable = Path(f"/proc/{value}/exe")
                                if executable.exists() and executable.resolve() == args.helper.resolve():
                                    return int(value)
                            return None

                        pid = wait_for(worker_pid)
                        os.kill(pid, signal.SIGKILL)  # 只终止隔离测试应用中已确认的辅助子进程。
                        assert wait_for(lambda: finished(crashed))["state"] == "failed"
                        wait_for(lambda: not command("get_ocr_status")["running"])
                        recovered = submit("light.jpg")
                        result = wait_for(lambda: finished(recovered), timeout=60)
                        assert result["state"] == "completed", result
                        print(json.dumps({"installed": True, "live_ipc": "passed", "get_status_while_ocr_s": round(responsive_s, 4)}))
                    assert command("quit")["ok"]
                    app.wait(timeout=10)
                    assert app.returncode == 0, app.returncode
                    print("optional package", installed, "passed")
                finally:
                    stop(app)
                wait_for(lambda: not endpoint.exists())
        finally:
            # 合成器可能持有私有 D-Bus 子进程，而非用户的会话总线。
            if sway.poll() is None:
                os.killpg(sway.pid, signal.SIGTERM)
            stop(sway)


if __name__ == "__main__":
    main()
