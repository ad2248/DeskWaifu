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

from gi.repository import Gio, GLib


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


def step(directory, action, expected, perform):
    directory.mkdir()
    report = {"input": action, "expected": expected, "status": "failed"}
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
        probe = subprocess.run([
            "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
            "stream=codec_name,avg_frame_rate,bit_rate,nb_frames", "-of", "json", str(video),
        ], check=True, capture_output=True, text=True)
        stream = json.loads(probe.stdout)["streams"][0]
        assert stream["codec_name"] == "hevc" and stream["avg_frame_rate"] == "15/1", stream
        assert int(stream["nb_frames"]) >= 15, stream
        report["video"] = stream
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        (directory / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--output", type=lambda value: Path(value).resolve(), required=True)
    args = parser.parse_args()
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    panel_argv = [str(args.bin_dir / "deskwaifu-terminal-panel")]
    close_action = {"action": "KWin close window", "window": "DeskWaifu 控制面板"}
    args.output.mkdir(parents=True, exist_ok=False)
    socket = Path("/tmp/deskwaifu.socket")
    assert not os.path.lexists(socket), f"DeskWaifu socket already exists at {socket}"

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
    def run_script(path, name):
        reply = subprocess.check_output(bus + ["/Scripting", "org.kde.kwin.Scripting", "loadScript", "ss",
                                               str(path), name], text=True)
        script_id = int(reply.split()[-1])
        assert script_id >= 0, reply
        return subprocess.run(bus + [f"/Scripting/Script{script_id}", "org.kde.kwin.Script", "run"],
                              check=True, capture_output=True, text=True)

    run_script(observer, "deskwaifu-panel-observer")
    kwin_log = runtime / "kwin.log"

    def observed(marker):
        return marker in kwin_log.read_text(errors="replace")

    panel = None
    opening = args.output / "01-open-panel"
    with (args.output / "host.log").open("wb") as host_log:
        host = subprocess.Popen([str(args.bin_dir / "deskwaifu")], stdout=host_log, stderr=subprocess.STDOUT)
        try:
            wait_for("DeskWaifu host socket", lambda: host.poll() is None and socket.is_socket())

            def open_panel():
                nonlocal panel
                with (opening / "panel.stdout.log").open("wb") as stdout, (opening / "panel.stderr.log").open("wb") as stderr:
                    panel = subprocess.Popen(panel_argv, stdout=stdout, stderr=stderr)
                wait_for("KWin to show the panel window",
                         lambda: panel.poll() is None and observed("DESKWAIFU_TEST_WINDOW_OPEN"))
                return {"window": "DeskWaifu 控制面板", "kwin_event": "windowAdded", "process_running": True}

            step(opening, panel_argv, "The DeskWaifu control panel window appears", open_panel)
            closing = args.output / "02-close-panel"

            def close_panel():
                close_script = runtime / "close-panel.js"
                close_script.write_text('''const panel = workspace.windowList().find(w => w.caption === "DeskWaifu 控制面板");
if (panel) panel.closeWindow();
''')
                result = run_script(close_script, "deskwaifu-panel-close")
                (closing / "close.stdout.log").write_text(result.stdout)
                (closing / "close.stderr.log").write_text(result.stderr)
                wait_for("KWin to remove the panel window", lambda: observed("DESKWAIFU_TEST_WINDOW_CLOSED"))
                return {"kwin_event": "windowRemoved", "input_exit_code": result.returncode}

            step(closing, close_action, "The DeskWaifu control panel window disappears", close_panel)
            print(f"PASS: open and close panel evidence in {args.output}")
        finally:
            if panel is not None:
                stop(panel)
            stop(host)
            (args.output / "kwin.log").write_bytes(kwin_log.read_bytes())


if __name__ == "__main__":
    main()
