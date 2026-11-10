#!/usr/bin/env python3
"""Offline QAudioSource -> ASR -> production desktop clipboard smoke, container only."""
import argparse
import array
import base64
import http.server
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import wave

TEXT = "  中文语音测试\n第二行\t保留空白  \n"
SENTINEL = "原剪贴板\n 不得覆盖 \t".encode()
API_PATH = "/api/v1/services/aigc/multimodal-generation/generation"
KEY = "smoke-offline-not-a-real-key"


def wait_for(predicate, description, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise TimeoutError(description)


def interrupt(signum, _frame):
    # A second termination signal must not interrupt the cleanup's SIGKILL pass.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    raise InterruptedError(f"smoke interrupted by signal {signum}")


def marker_score(samples):
    """Phase-independent 1kHz energy fraction in a 16kHz PCM window."""
    energy = sum(value * value for value in samples)
    if not energy:
        return 0.0
    real = sum(value * math.cos(2 * math.pi * (index % 16) / 16)
               for index, value in enumerate(samples))
    imaginary = sum(value * math.sin(2 * math.pi * (index % 16) / 16)
                    for index, value in enumerate(samples))
    return 2 * (real * real + imaginary * imaginary) / (len(samples) * energy)


class Smoke:
    def __init__(self, args, root):
        self.args, self.root = args, root
        self.processes = []
        self.records, self.errors = [], []
        self.mode = "success"
        # Deliberately do not inherit host display, audio, config, proxy or API-key variables.
        self.env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8",
                    "HOME": str(root / "home"), "XDG_RUNTIME_DIR": str(root / "run"),
                    "XDG_CONFIG_HOME": str(root / "config"), "XDG_CACHE_HOME": str(root / "cache"),
                    "XDG_DATA_HOME": str(root / "data"), "WLR_BACKENDS": "headless",
                    "WLR_HEADLESS_OUTPUTS": "1", "WLR_LIBINPUT_NO_DEVICES": "1",
                    "WLR_RENDERER": "pixman", "LIBGL_ALWAYS_SOFTWARE": "1",
                    "GALLIUM_DRIVER": "llvmpipe", "QT_QPA_PLATFORM": "wayland",
                    "QT_AUDIO_BACKEND": "pulseaudio", "PULSE_SOURCE": "test-mic",
                    "PIPEWIRE_REMOTE": "pipewire-0", "NO_PROXY": "*", "no_proxy": "*"}
        for name in ("home", "run", "config", "cache", "data"):
            (root / name).mkdir(mode=0o700)
        self.env["PULSE_SERVER"] = f"unix:{root}/run/pulse/native"
        self.env["PULSE_RUNTIME_PATH"] = str(root / "run/pulse")
        self.env["PIPEWIRE_RUNTIME_DIR"] = str(root / "run")
        self.env["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={root}/run/bus"

    def run(self, *command, **kwargs):
        return subprocess.run(command, env=self.env, check=True, capture_output=True,
                              timeout=kwargs.pop("timeout", 10), **kwargs)

    def spawn(self, name, command, extra=None, stdin=subprocess.DEVNULL):
        with (self.args.logs / f"{name}.log").open("wb") as log:
            process = subprocess.Popen(command, env=dict(self.env, **(extra or {})),
                                       stdin=stdin, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
        self.processes.append(process)
        return process

    def stop(self):
        errors = []
        # Every spawn creates its own session: pid is the registered PGID even
        # after the leader exits. A living grandchild may ignore SIGTERM.
        for sig in (signal.SIGTERM, signal.SIGKILL):
            for process in reversed(self.processes):
                try:
                    os.killpg(process.pid, sig)
                except ProcessLookupError:
                    pass
                except OSError as error:
                    errors.append(error)
            for process in reversed(self.processes):
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired as error:
                    if sig == signal.SIGKILL:
                        errors.append(error)
                except OSError as error:
                    errors.append(error)
        if errors:
            raise errors[0]

    def cleanup(self):
        try:
            self.stop()
        finally:
            try:
                if hasattr(self, "server"):
                    try:
                        self.server.shutdown()
                    finally:
                        self.server.server_close()
            finally:
                (self.args.logs / "mock-requests.json").write_text(json.dumps(
                    {"requests": self.records, "errors": self.errors}, indent=2))

    def command(self, name, **parameters):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(5)
            connection.connect(str(self.root / "run/deskwaifu.sock"))
            connection.sendall(json.dumps(dict(command=name, **parameters)).encode() + b"\n")
            with connection.makefile("rb") as stream:
                return json.loads(stream.readline())

    def checked(self, name, **parameters):
        reply = self.command(name, **parameters)
        assert reply.get("ok"), (name, reply)
        return reply

    def start_mock(self):
        smoke = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                try:
                    assert self.path == API_PATH, self.path
                    assert self.headers.get("Authorization") == f"Bearer {KEY}"
                    assert self.headers.get("X-DashScope-SSE") == "disable"
                    length = int(self.headers["Content-Length"])
                    assert 0 < length < 15 * 1024 * 1024
                    body = json.loads(self.rfile.read(length))
                    assert body["model"] == "qwen-audio-3.0-asr-flash"
                    assert body["parameters"] == {"format": "wav", "sample_rate": "16000"}
                    content = body["input"]["messages"][0]["content"][0]
                    assert content["type"] == "input_audio"
                    uri = content["input_audio"]["data"]
                    assert uri.startswith("data:audio/wav;base64,")
                    audio = base64.b64decode(uri.split(",", 1)[1], validate=True)
                    with wave.open(io.BytesIO(audio), "rb") as wav:
                        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 16000)
                        pcm = wav.readframes(wav.getnframes())
                    samples = array.array("h", pcm)
                    if os.sys.byteorder != "little":
                        samples.byteswap()
                    assert len(samples) >= 5600, "less than 100ms captured plus 250ms padding"
                    assert all(value == 0 for value in samples[-4000:]), "WAV missing exact 250ms zero padding"
                    captured = samples[:-4000]
                    peak = max(abs(value) for value in captured)
                    assert peak > 1000, "actual capture is silent"
                    index = len(smoke.records)
                    (smoke.args.logs / f"request-{index}.wav").write_bytes(audio)
                    smoke.records.append({"samples": len(samples), "captured_samples": len(captured),
                                          "padding_samples": 4000, "peak": peak,
                                          "capture_tail_peak": max(abs(v) for v in captured[-4000:]),
                                          "capture_tail_1khz_score": marker_score(captured[-4000:]),
                                          "mode": smoke.mode})
                    status = 503 if smoke.mode == "failure" else 200
                    response = {"error": "intentional offline failure"} if status == 503 else {"text": TEXT}
                except Exception as error:
                    smoke.errors.append(repr(error))
                    status, response = 400, {"error": "mock validation failed"}
                encoded = json.dumps(response, ensure_ascii=False).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.env["DESKWAIFU_TEST_ASR_TRANSPORT"] = f"http://127.0.0.1:{self.server.server_port}"

    def start_services(self):
        self.spawn("dbus", ["dbus-daemon", "--session", "--nofork", "--nopidfile",
                            f"--address={self.env['DBUS_SESSION_BUS_ADDRESS']}"])
        wait_for(lambda: (self.root / "run/bus").is_socket(), "private D-Bus")
        # Hardware monitors must never discover a host mic, even in a misconfigured container.
        wp = self.root / "config/wireplumber/wireplumber.conf.d/no-hardware.conf"
        wp.parent.mkdir(parents=True)
        wp.write_text('wireplumber.profiles = { main = { monitor.alsa = disabled\n'
                      'monitor.bluez = disabled\n monitor.v4l2 = disabled\n monitor.libcamera = disabled } }\n')
        self.spawn("pipewire", ["pipewire"])
        wait_for(lambda: (self.root / "run/pipewire-0").is_socket(), "private PipeWire")
        self.spawn("wireplumber", ["wireplumber"])
        self.spawn("pipewire-pulse", ["pipewire-pulse"])
        wait_for(lambda: (self.root / "run/pulse/native").is_socket(), "private Pulse server")
        self.spawn("virtual-mic", ["pw-loopback",
                   "--capture-props=media.class=Audio/Sink node.name=test-sink node.description=test-sink",
                   "--playback-props=media.class=Audio/Source node.name=test-mic node.description=test-mic"])

        wait_for(self.select_mic, "default private virtual mic")
        config = self.root / "sway.config"
        config.write_text("xwayland disable\nseat seat0 fallback true\noutput * resolution 800x600\n")
        self.spawn("sway", ["sway", "--config", str(config)])
        display = wait_for(lambda: next((p for p in (self.root / "run").glob("wayland-*")
                                        if p.is_socket()), None), "private Sway")
        self.env["WAYLAND_DISPLAY"] = display.name
        sway_socket = wait_for(lambda: next((p for p in (self.root / "run").glob("sway-ipc.*.sock")
                                            if p.is_socket()), None), "private Sway IPC")
        self.env["SWAYSOCK"] = str(sway_socket)
        self.run("swaymsg", "workspace", "voice-smoke-empty")

    def select_mic(self):
        try:
            status = self.run("wpctl", "status", "--name", text=True).stdout
            match = re.search(r"(\d+)\.\s+test-mic", status)
            if not match:
                return False
            self.run("wpctl", "set-default", match[1])
            return 'test-mic' in self.run("wpctl", "inspect", "@DEFAULT_AUDIO_SOURCE@", text=True).stdout
        except subprocess.CalledProcessError:
            # WirePlumber publishes node/default metadata asynchronously; the
            # bounded startup poll must wait for inspect to resolve the default.
            return False

    def start_app(self):
        plugins = self.root / "plugins"
        for name, filename in (("desktop", "desktop.so"), ("ipc", "ipc.so"),
                               ("voice_input", "voice_input_smoke.so")):
            destination = plugins / name
            destination.mkdir(parents=True)
            shutil.copy2(self.args.build / "plugins" / name / filename, destination / f"{name}.so")
        models = self.root / "models"
        models.mkdir()
        shutil.copytree(self.args.resources / "Haru", models / "Haru")
        self.env["DESKWAIFU_PLUGIN_DIR"] = str(plugins)
        self.app = self.spawn("desktop", [str(self.args.build / "bin/deskwaifu"), "--models_dir", str(models)],
                              {"WAYLAND_DEBUG": "client"})
        wait_for(lambda: (self.root / "run/deskwaifu.sock").is_socket(), "desktop IPC socket")
        wait_for(lambda: self.command("get_voice_input_status").get("ok"), "voice plugin ready")
        devices = self.checked("list_voice_input_devices")["devices"]
        mic = next((d for d in devices if d["name"] == "test-mic"), None)
        assert mic is not None, devices
        self.checked("set_voice_input_settings", api_key=KEY, device_id=mic["id"],
                     endpoint="https://asr.invalid" + API_PATH, model="qwen-audio-3.0-asr-flash",
                     max_recording_seconds=15)
        # The fixture overrides transport only; production settings must still reject HTTP.
        assert not self.command("set_voice_input_settings", endpoint="http://127.0.0.1")['ok']
        self.no_focus()

    def no_focus(self):
        tree = json.loads(self.run("swaymsg", "-t", "get_tree", "-r").stdout)

        def walk(node):
            yield node
            for child in node.get("nodes", []) + node.get("floating_nodes", []):
                yield from walk(child)

        focused = [n for n in walk(tree) if n.get("focused")]
        assert focused and all(n["type"] in ("workspace", "output", "root") for n in focused), focused
        assert not any(n.get("app_id") or n.get("window") for n in walk(tree)), tree
        log = (self.args.logs / "desktop.log").read_text(errors="replace")
        assert re.search(r"set_keyboard_interactivity\(0\)", log), "desktop layer must use keyboard interactivity none"
        assert "wl_keyboard" not in "\n".join(line for line in log.splitlines() if ".enter(" in line), "desktop received keyboard focus"
        (self.args.logs / "focus-tree.json").write_text(json.dumps(tree, ensure_ascii=False))

    def paste(self):
        return self.run("wl-paste", "--no-newline", "--type", "text/plain;charset=utf-8").stdout

    def seed(self):
        seed_file = self.root / "clipboard-seed.txt"
        seed_file.write_bytes(SENTINEL)
        with seed_file.open("rb") as source:
            holder = self.spawn("clipboard-seed", ["wl-copy", "--foreground", "--type",
                                                  "text/plain;charset=utf-8"], stdin=source)

        def ready():
            assert holder.poll() is None, "foreground clipboard seed exited before selection was ready"
            try:
                return self.paste() == SENTINEL
            except subprocess.CalledProcessError:
                return False  # The new private seat may not yet have a selection.

        wait_for(ready, "foreground clipboard seed", timeout=10)

    def play(self, filename):
        self.run("pw-play", "--target=test-sink", str(filename), timeout=20)

    def begin(self):
        reply = self.checked("start_voice_input")
        assert reply["phase"] == "recording", reply
        return reply["session_id"]

    def result(self, session):
        wait_for(lambda: self.checked("get_voice_input_status")["phase"] == "idle", "recognition completion")
        return self.checked("get_voice_input_result", session_id=session)

    def check(self):
        # Finish playback with a distinctive 250ms, 1kHz marker, not trailing silence.
        fixture = Path(__file__).with_name("fixtures") / "voice-input.pcm"
        marker = array.array("h", [int(12000 * math.sin(2 * math.pi * 1000 * n / 16000)) for n in range(4000)])
        if os.sys.byteorder != "little":
            marker.byteswap()
        audio = self.root / "speech-tail.wav"
        with wave.open(str(audio), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(fixture.read_bytes() + marker.tobytes())
        self.seed()
        session = self.begin()
        self.play(audio)
        self.checked("stop_voice_input", session_id=session)
        result = self.result(session)
        assert result["last_outcome"] == "copied" and result["copy_state"] == "committed", result
        assert result["text"].encode() == TEXT.encode(), result
        wait_for(lambda: self.paste() == TEXT.encode(), "byte-exact Chinese clipboard")
        assert len(self.records) == 1, self.records
        first = self.records[0]
        assert first["capture_tail_peak"] > 1000 and first["capture_tail_1khz_score"] >= .35, first
        expected_samples = fixture.stat().st_size // 2 + 4000
        assert expected_samples - 8000 <= first["captured_samples"] <= expected_samples + 8000, first
        self.no_focus()

        self.seed()
        previous = len(self.records)
        session = self.begin()
        self.play(audio)
        self.checked("cancel_voice_input", session_id=session)
        assert self.result(session)["last_outcome"] == "cancelled"
        assert len(self.records) == previous and self.paste() == SENTINEL

        self.mode = "failure"
        session = self.begin()
        self.play(audio)
        self.checked("stop_voice_input", session_id=session)
        result = self.result(session)
        assert result["last_outcome"] == "failed" and result["error_code"] == "asr_http", result
        assert self.paste() == SENTINEL and len(self.records) == previous + 1

        self.mode = "success"
        self.checked("set_voice_input_settings", max_recording_seconds=2)
        # Continuous non-silent input crosses the timer boundary; do not send Stop.
        long_audio = self.root / "long.wav"
        with wave.open(str(long_audio), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(marker.tobytes() * 16)
        started = time.monotonic()
        session = self.begin()
        self.play(long_audio)
        result = self.result(session)
        assert time.monotonic() - started < 12
        assert result["last_outcome"] == "copied" and 1500 <= result["elapsed_ms"] <= 3500, result
        assert len(self.records) == previous + 2 and self.paste() == TEXT.encode()
        automatic = self.records[-1]
        assert 24000 <= automatic["captured_samples"] <= 32000, automatic
        assert automatic["samples"] <= 36000 and automatic["padding_samples"] == 4000, automatic
        self.no_focus()
        assert not self.errors, self.errors
        assert self.app.poll() is None, "desktop exited prematurely"
        self.checked("quit")
        self.app.wait(timeout=10)
        assert self.app.returncode == 0
        print("PASS: real capture, offline ASR, unfocused desktop clipboard, byte fidelity, cancel/failure, automatic limit")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=lambda p: Path(p).resolve())
    parser.add_argument("resources", type=lambda p: Path(p).resolve())
    parser.add_argument("--logs", type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.logs = args.logs or args.build / "test-logs/voice-input"
    args.logs.mkdir(parents=True, exist_ok=True)
    # This test launches a compositor; never use it as a host desktop test.
    assert Path("/run/.containerenv").exists() or Path("/.dockerenv").exists(), "run inside a private CI container"
    for tool in ("dbus-daemon", "sway", "swaymsg", "pipewire", "pipewire-pulse", "wireplumber",
                 "pw-loopback", "pw-play", "wpctl", "wl-copy", "wl-paste"):
        assert shutil.which(tool), f"required dependency missing: {tool}"
    with tempfile.TemporaryDirectory(prefix="voice-smoke-") as directory:
        smoke = Smoke(args, Path(directory))
        previous_handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
        try:
            for sig in previous_handlers:
                signal.signal(sig, interrupt)
            smoke.start_mock()
            smoke.start_services()
            smoke.start_app()
            smoke.check()
        finally:
            try:
                smoke.cleanup()
            finally:
                for sig, handler in previous_handlers.items():
                    signal.signal(sig, handler)


if __name__ == "__main__":
    main()
