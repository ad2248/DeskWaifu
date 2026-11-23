#!/usr/bin/env python3
"""Record and verify opening and closing the GPUI panel in an isolated Plasma session."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import tempfile
from concurrent.futures import ThreadPoolExecutor
from threading import Event

import sys

CASE = "GUI在无守护进程情况下启动关闭"
OPEN = "启动"
CLOSE = "关闭"
TITLE = "DeskWaifu 控制面板"
DEFAULT_SOCKET = Path("/tmp/deskwaifu.socket")
CLOSE_SCRIPT = '''const panel = workspace.windowList().find(w => w.caption === "DeskWaifu 控制面板");
if (panel) panel.closeWindow();
'''


def write_report(directory, report):
    (directory / "result.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def wait_for(description, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {description}")


def stop(process, sig=signal.SIGTERM):
    if process.poll() is None:
        process.send_signal(sig)
    try:
        return process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise


def capture_frame(bus):
    from gi.repository import Gio, GLib

    with tempfile.TemporaryFile() as frame:
        fds = Gio.UnixFDList.new()
        handle = fds.append(frame.fileno())
        reply, _ = bus.call_with_unix_fd_list_sync(
            "org.kde.KWin.ScreenShot2", "/org/kde/KWin/ScreenShot2",
            "org.kde.KWin.ScreenShot2", "CaptureWorkspace",
            GLib.Variant("(a{sv}h)", ({}, handle)), None,
            Gio.DBusCallFlags.NO_AUTO_START, 3000, fds, None,
        )
        metadata = reply.unpack()[0]
        assert (metadata["width"], metadata["height"], metadata["stride"]) == (1280, 800, 5120), metadata
        assert metadata["format"] in (4, 5, 6, 16, 17, 18), metadata
        size = 5120 * 800
        wait_for("screenshot pixels", lambda: os.fstat(frame.fileno()).st_size == size, timeout=3)
        frame.seek(0)
        pixels = frame.read(size)
        # Qt's RGB32 family is BGRA in memory on x86_64. RGBX8888 is RGBA.
        if metadata["format"] in (4, 5, 6):
            converted = bytearray(pixels)
            converted[0::4], converted[2::4] = pixels[2::4], pixels[0::4]
            return converted
        return pixels


def record_frames(stream, ready, finished):
    from gi.repository import Gio

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    started = time.monotonic()
    count = 0
    while not finished.is_set():
        frame = capture_frame(bus)
        # Fill any missed capture slots with the latest compositor frame.
        target = max(count + 1, int((time.monotonic() - started) * 15))
        for _ in range(count, target):
            stream.write(frame)
        count = target
        ready.set()
        finished.wait(max(0, started + count / 15 - time.monotonic()))


def step(directory, report, perform):
    report["status"] = "failed"
    video = directory / "screen.mp4"
    try:
        with (directory / "recording.stderr.log").open("wb") as log:
            recorder = subprocess.Popen([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
                "-pixel_format", "rgba", "-video_size", "1280x800", "-framerate", "15",
                "-i", "pipe:0", "-an", "-c:v", "libx265", "-preset", "ultrafast",
                "-x265-params", "pools=2:frame-threads=2:log-level=error", "-crf", "27",
                "-maxrate", "450k", "-bufsize", "900k", "-pix_fmt", "yuv420p",
                "-tag:v", "hvc1", "-movflags", "+faststart", str(video),
            ], stdin=subprocess.PIPE, stderr=log)
            ready, finished = Event(), Event()
            with ThreadPoolExecutor(max_workers=1) as executor:
                recording = executor.submit(record_frames, recorder.stdin, ready, finished)
                try:
                    def recording_ready():
                        if recording.done():
                            recording.result()
                        return ready.is_set()

                    wait_for("first recorded frame", recording_ready)
                    time.sleep(0.6)
                    report["output"] = perform()
                    time.sleep(1.5)  # Keep the verified result visible in the recording.
                finally:
                    finished.set()
                    try:
                        recording.result()
                    finally:
                        recorder.stdin.close()
                        if recorder.wait(timeout=10) != 0:
                            raise RuntimeError("FFmpeg failed; see recording.stderr.log")
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        if isinstance(error, subprocess.CalledProcessError):
            report["exit_code"] = error.returncode
        raise
    finally:
        write_report(directory, report)


def run_case(output, reports):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    opening, closing = output / OPEN, output / CLOSE
    socket = runtime / "absent.sock"
    open_report, close_report = reports
    open_report["input"]["env"] = {"DESKWAIFU_PANEL_SOCKET": str(socket)}
    if any(os.path.lexists(path) for path in (socket, DEFAULT_SOCKET)):
        raise RuntimeError("用例要求私有 socket 和默认宿主 socket 均不存在")

    observer = runtime / "observe-panel.js"
    observer.write_text('''
const panel = window => window.caption === "DeskWaifu 控制面板";
let observed = null;
workspace.windowAdded.connect(window => {
    if (panel(window)) {
        observed = window;
        print("DESKWAIFU_TEST_WINDOW_OPEN");
    }
});
workspace.windowRemoved.connect(window => {
    if (window === observed) print("DESKWAIFU_TEST_WINDOW_CLOSED");
});
''')
    bus = ["busctl", "--user", "--timeout=3", "call", "org.kde.KWin"]
    def run_script(path, name, output=None):
        load = bus + ["/Scripting", "org.kde.kwin.Scripting", "loadScript", "ss", str(path), name]
        result = subprocess.run(load, capture_output=True, text=True)
        if output is not None:
            (output / "stdout.log").write_text(result.stdout)
            (output / "stderr.log").write_text(result.stderr)
        result.check_returncode()
        script_id = int(result.stdout.split()[-1])
        if script_id < 0:
            raise RuntimeError(f"KWin rejected {name}: {result.stdout}")
        run = bus + [f"/Scripting/Script{script_id}", "org.kde.kwin.Script", "run"]
        if output is None:
            return subprocess.run(run, check=True).returncode
        with (output / "stdout.log").open("ab") as stdout, (output / "stderr.log").open("ab") as stderr:
            return subprocess.run(run, stdout=stdout, stderr=stderr, check=True).returncode

    run_script(observer, "deskwaifu-panel-observer")
    kwin_log = runtime / "kwin.log"

    def observed(marker):
        return marker in kwin_log.read_text(errors="replace")

    panel = None

    def open_panel():
        nonlocal panel
        action = open_report["input"]
        with (opening / "stdout.log").open("wb") as stdout, (opening / "stderr.log").open("wb") as stderr:
            panel = subprocess.Popen(action["argv"], env=os.environ | action["env"], stdout=stdout, stderr=stderr)

        def window_open():
            if panel.poll() is not None:
                raise subprocess.CalledProcessError(panel.returncode, action["argv"])
            return observed("DESKWAIFU_TEST_WINDOW_OPEN")

        wait_for("KWin to show the panel window", window_open)
        if any(os.path.lexists(path) for path in (socket, DEFAULT_SOCKET)):
            raise RuntimeError("启动面板时出现了宿主 socket")
        return {"window": TITLE, "kwin_event": "windowAdded", "daemon_socket_present": False}

    def close_panel():
        close_script = runtime / "close-panel.js"
        close_script.write_text(CLOSE_SCRIPT, encoding="utf-8")
        result = {"exit_code": run_script(close_script, "deskwaifu-panel-close", closing)}
        close_report["output"] = result
        wait_for("KWin to remove the panel window", lambda: observed("DESKWAIFU_TEST_WINDOW_CLOSED"))
        result.update(window=TITLE, kwin_event="windowRemoved")
        return result

    try:
        step(opening, open_report, open_panel)
        step(closing, close_report, close_panel)
    finally:
        if panel is not None:
            stop(panel)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--output", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--in-session", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    output = args.output / CASE
    opening, closing = output / OPEN, output / CLOSE
    reports = (
        {"order": 1, "input": {"argv": [str(args.bin_dir / "deskwaifu-terminal-panel")]},
         "expected": "没有宿主时，控制面板窗口出现", "status": "not_run"},
        {"order": 2, "input": {"action": "KWin close window", "window": TITLE, "script": CLOSE_SCRIPT},
         "expected": "同一个控制面板窗口消失", "status": "not_run", "reason": "等待启动步骤通过"},
    )
    if args.in_session:
        reports[1].pop("reason")
        run_case(output, reports)
        return 0

    output.mkdir(parents=True)
    for directory, report in zip((opening, closing), reports):
        directory.mkdir()
        write_report(directory, report)
    launcher = Path(__file__).resolve().parents[1] / "scripts" / "with-headless-plasma.sh"
    command = ["bash", str(launcher), sys.executable, str(Path(__file__).resolve()),
               "--bin-dir", str(args.bin_dir), "--output", str(args.output), "--in-session"]
    session_log = opening / "session.log"
    with session_log.open("wb") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=os.environ | {
            "KWIN_SCREENSHOT_NO_PERMISSION_CHECK": "1",
            "QT_LOGGING_RULES": "kwin_scripting.debug=true",
        })
    if result.returncode != 0:
        if json.loads((opening / "result.json").read_text())["status"] == "not_run":
            reports[0].update(status="failed", error="用例初始化失败；详见 session.log")
            write_report(opening, reports[0])
        print(session_log.read_text(errors="replace"), file=sys.stderr, end="")
        return result.returncode if result.returncode > 0 else 128 - result.returncode
    print(f"PASS: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
