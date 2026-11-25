"""在私有无头 Plasma 会话中记录控制面板的启动与关闭。

用例不启动 DeskWaifu 宿主，只启动安装包提供的控制面板，并给它一个不存在的私有
socket。每一步都用 KWin 截图录制 15 fps H.265 视频作为证据；视频规格由人工下载后核验。
"""
import os
import signal
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Event

from record import Case

CASE = Case(
    id="gui_panel_open_close",
    title="GUI在无守护进程情况下启动关闭",
    requires=frozenset({"session", "drm"}),
)
TITLE = "DeskWaifu 控制面板"
DEFAULT_SOCKET = Path("/tmp/deskwaifu.socket")
CLOSE_SCRIPT = """const panel = workspace.windowList().find(w => w.caption === "DeskWaifu 控制面板");
if (panel) panel.closeWindow();
"""
OBSERVER_SCRIPT = """const panel = window => window.caption === "DeskWaifu 控制面板";
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
"""


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


class Recorder:
    """在步骤执行期间以 15 fps 录制合成器画面。"""

    def __init__(self, directory):
        self.directory = Path(directory)

    def __enter__(self):
        self._log = (self.directory / "recording.stderr.log").open("wb")
        self._encoder = subprocess.Popen([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
            "-pixel_format", "rgba", "-video_size", "1280x800", "-framerate", "15",
            "-i", "pipe:0", "-an", "-c:v", "libx265", "-preset", "ultrafast",
            "-x265-params", "pools=2:frame-threads=2:log-level=error", "-crf", "27",
            "-maxrate", "450k", "-bufsize", "900k", "-pix_fmt", "yuv420p",
            "-tag:v", "hvc1", "-movflags", "+faststart", str(self.directory / "screen.mp4"),
        ], stdin=subprocess.PIPE, stderr=self._log)
        self._ready = Event()
        self._finished = Event()
        self._pool = ThreadPoolExecutor(max_workers=1)
        self._task = self._pool.submit(record_frames, self._encoder.stdin, self._ready, self._finished)
        wait_for("first recorded frame", self._first_frame)
        return self

    def _first_frame(self):
        if self._task.done():
            self._task.result()
        return self._ready.is_set()

    def __exit__(self, error_type, error, traceback):
        self._finished.set()
        try:
            self._task.result()
        finally:
            self._encoder.stdin.close()
            status = self._encoder.wait(timeout=10)
            self._pool.shutdown(wait=True)
            self._log.close()
        if status != 0 and error is None:
            raise RuntimeError("FFmpeg failed; see recording.stderr.log")
        return False


def kwin_reported(runtime, marker):
    return marker in (runtime / "kwin.log").read_text(errors="replace")


def run_kwin_script(runtime, path, name, step=None):
    bus = ["busctl", "--user", "--timeout=3", "call", "org.kde.KWin"]
    load = bus + ["/Scripting", "org.kde.kwin.Scripting", "loadScript", "ss", str(path), name]
    result = subprocess.run(load, capture_output=True, text=True)
    if step is not None:
        step.log("stdout.log", result.stdout)
        step.log("stderr.log", result.stderr)
    result.check_returncode()
    script_id = int(result.stdout.split()[-1])
    if script_id < 0:
        raise RuntimeError(f"KWin rejected {name}: {result.stdout}")
    run = bus + [f"/Scripting/Script{script_id}", "org.kde.kwin.Script", "run"]
    completed = subprocess.run(run, capture_output=True, text=True)
    if step is None:
        completed.check_returncode()
        return completed.returncode
    with step.open("stdout.log", "ab") as stdout, step.open("stderr.log", "ab") as stderr:
        stdout.write(completed.stdout.encode())
        stderr.write(completed.stderr.encode())
    completed.check_returncode()
    return completed.returncode


def start_panel(binary, socket, step):
    argv = [str(binary)]
    environment = os.environ | {"DESKWAIFU_PANEL_SOCKET": str(socket)}
    with step.open("stdout.log") as stdout, step.open("stderr.log") as stderr:
        panel = subprocess.Popen(argv, env=environment, stdout=stdout, stderr=stderr)

    def window_open():
        if panel.poll() is not None:
            raise subprocess.CalledProcessError(panel.returncode, argv)
        return kwin_reported(Path(os.environ["XDG_RUNTIME_DIR"]), "DESKWAIFU_TEST_WINDOW_OPEN")

    wait_for("KWin to show the panel window", window_open)
    if any(os.path.lexists(path) for path in (socket, DEFAULT_SOCKET)):
        raise RuntimeError("启动面板时出现了宿主 socket")
    return panel


def close_panel(runtime, step):
    script = runtime / "close-panel.js"
    script.write_text(CLOSE_SCRIPT, encoding="utf-8")
    result = {"exit_code": run_kwin_script(runtime, script, "deskwaifu-panel-close", step)}
    wait_for("KWin to remove the panel window", lambda: kwin_reported(runtime, "DESKWAIFU_TEST_WINDOW_CLOSED"))
    result.update(window=TITLE, kwin_event="windowRemoved")
    return result


def run(context):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    socket = runtime / "absent.sock"
    if any(os.path.lexists(path) for path in (socket, DEFAULT_SOCKET)):
        raise RuntimeError("用例要求私有 socket 和默认宿主 socket 均不存在")

    observer = runtime / "observe-panel.js"
    observer.write_text(OBSERVER_SCRIPT, encoding="utf-8")
    run_kwin_script(runtime, observer, "deskwaifu-panel-observer")

    panel = None
    try:
        binary = context.bin("deskwaifu-terminal-panel")
        with context.step(
            1, "启动", "没有宿主时，控制面板窗口出现",
            {"argv": [str(binary)], "env": {"DESKWAIFU_PANEL_SOCKET": str(socket)}},
        ) as step:
            with Recorder(step.directory):
                time.sleep(0.6)
                panel = start_panel(binary, socket, step)
                time.sleep(1.5)  # Keep the verified result visible in the recording.
            step.output = {"window": TITLE, "kwin_event": "windowAdded", "daemon_socket_present": False}
        with context.step(
            2, "关闭", "同一个控制面板窗口消失",
            {"action": "KWin close window", "window": TITLE, "script": CLOSE_SCRIPT},
        ) as step:
            with Recorder(step.directory):
                time.sleep(0.6)
                step.output = close_panel(runtime, step)
                time.sleep(1.5)  # Keep the verified result visible in the recording.
    finally:
        if panel is not None:
            stop(panel)
