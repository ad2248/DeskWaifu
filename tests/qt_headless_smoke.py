"""Opt-in Qt backend smoke test on a private two-output KWin and D-Bus session.

Usage: python3 tests/qt_headless_smoke.py BUILD_DIR SDK_ROOT [--scale 1.5]
Logs are retained in BUILD_DIR/qt-smoke-logs. No user's desktop/config is touched.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def assert_layer_shell(trace):
    # Protocol advertisement and QWindow properties do not prove the window role.
    assert re.search(r"zwlr_layer_shell_v1[@#]\d+\.get_layer_surface\(", trace), \
        "Model window did not actually acquire a layer-shell role"
    assert not re.search(r"xdg_wm_base[@#]\d+\.get_xdg_surface\(", trace), \
        "Model window fell back to an ordinary xdg-shell window"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("--scale", default="1")
    parser.add_argument("--native", action="store_true", help="Run the model/IPC baseline with the native backend")
    args = parser.parse_args()
    for program in ("kwin_wayland", "dbus-run-session"):
        if not shutil.which(program):
            print(f"Missing {program}", file=sys.stderr)
            return 77
    if not os.environ.get("DESKWAIFU_QT_TEST_DBUS"):
        isolated_env = dict(os.environ, DESKWAIFU_QT_TEST_DBUS="1", DISPLAY="",
                            WAYLAND_DISPLAY="qt-model-test", QT_QPA_PLATFORMTHEME="generic",
                            QT_NO_XDG_DESKTOP_PORTAL="1")
        return subprocess.call(["dbus-run-session", "--", sys.executable,
                                str(Path(__file__).resolve()), *sys.argv[1:]], env=isolated_env)

    build = args.build.resolve()
    resources = args.sdk.resolve() / "Samples/Resources"
    assert (resources / "Haru/Haru.model3.json").is_file(), resources
    logs = build / "qt-smoke-logs" / args.scale
    logs.mkdir(parents=True, exist_ok=True)
    temp_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="qt-model-", dir=temp_root) as directory:
        root = Path(directory)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        config = root / "config/deskwaifu"
        config.mkdir(parents=True)
        (config / "config.json").write_text(json.dumps({"default_model": "Haru"}))
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CACHE_HOME=str(root / "cache"), XDG_DATA_HOME=str(root / "data"),
                   LIBGL_ALWAYS_SOFTWARE="1", MESA_DEBUG="context",
                   QT_LOGGING_RULES="qt.qpa.wayland.warning=true")
        # Never inherit a real compositor's control sockets or Qt shell override.
        for key in ("SWAYSOCK", "HYPRLAND_INSTANCE_SIGNATURE", "QT_WAYLAND_SHELL_INTEGRATION",
                    "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "QT_SCALE_FACTOR",
                    "QT_SCREEN_SCALE_FACTORS", "QT_SCALE_FACTOR_ROUNDING_POLICY"):
            env.pop(key, None)
        kwin = app = None
        with (logs / "kwin.log").open("w") as kwin_log, (logs / "model.log").open("w") as model_log:
            try:
                kwin = subprocess.Popen(["kwin_wayland", "--virtual", "--no-lockscreen",
                                         "--no-global-shortcuts", "--no-kactivities",
                                         "--width", "800", "--height", "600", "--output-count", "2",
                                         "--scale", args.scale, "--socket", "qt-model-test"],
                                        env=env, stdout=kwin_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 20
                while not (runtime / "qt-model-test").exists():
                    assert kwin.poll() is None, (logs / "kwin.log").read_text()
                    assert time.monotonic() < deadline, "KWin startup timed out"
                    time.sleep(.05)
                client_env = dict(env, WAYLAND_DISPLAY="qt-model-test", QT_QPA_PLATFORM="wayland",
                                  WAYLAND_DEBUG="client")
                if float(args.scale) != 1:
                    assert shutil.which("kscreen-doctor"), "kscreen-doctor is required to set actual output scaling"
                    # KWin's --scale controls virtual mode size, not the output's
                    # logical scale. Apply scaling through its output protocol.
                    screen_config = json.loads(subprocess.check_output(
                        ["kscreen-doctor", "-j"], env=dict(client_env, WAYLAND_DEBUG="0"), text=True))
                    changes = [f"output.{output['name']}.scale.{args.scale}"
                               for output in screen_config["outputs"]]
                    assert len(changes) == 2, screen_config
                    with (logs / "scale.log").open("w") as log:
                        subprocess.run(["kscreen-doctor", *changes], env=client_env,
                                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=15)
                client_env["DESKWAIFU_EXPECT_DPR"] = args.scale
                if shutil.which("kscreen-doctor"):
                    client_env["DESKWAIFU_QT_TEST_HOTPLUG"] = "1"
                window_log = ""
                if not args.native:
                    with (logs / "window.log").open("w") as log:
                        subprocess.run([str(build / "bin/qt-desktop-window-test")], env=client_env,
                                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
                    window_log = (logs / "window.log").read_text()
                    assert_layer_shell(window_log)
                    assert "outputs=2" in window_log, window_log[-3000:]
                    assert window_log.count("Output switch configured and committed") >= 2
                    if "DESKWAIFU_QT_TEST_HOTPLUG" in client_env:
                        assert "hotplug=1" in window_log
                # Ensure actual Wayland input regions include empty and nonempty states.
                populated = set()
                empty = nonempty = False
                for line in window_log.splitlines():
                    match = re.search(r"wl_region[@#](\d+)\.add\(", line)
                    if match:
                        populated.add(match[1])
                    match = re.search(r"set_input_region\(wl_region[@#](\d+)\)", line)
                    if match:
                        if match[1] in populated:
                            nonempty = True
                        else:
                            empty = True
                    match = re.search(r"wl_region[@#](\d+)\.destroy\(", line)
                    if match:
                        populated.discard(match[1])
                if not args.native:
                    assert empty and nonempty, "Qt did not submit both empty and shaped input regions"

                app = subprocess.Popen([str(build / "bin/deskwaifu"), "--models_dir", str(resources)],
                                       env=client_env, stdout=model_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 20
                while not (runtime / "deskwaifu.sock").exists():
                    assert app.poll() is None, (logs / "model.log").read_text()[-6000:]
                    assert time.monotonic() < deadline, "Model startup timed out"
                    time.sleep(.05)
                # Keep one connection throughout edit/stream transactions.
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(10)
                    connection.connect(str(runtime / "deskwaifu.sock"))
                    stream = connection.makefile("rb")

                    def request(command, **values):
                        connection.sendall((json.dumps(dict(command=command, **values)) + "\n").encode())
                        response = json.loads(stream.readline())
                        assert response["ok"], (command, response)
                        return response

                    request("get_status")
                    request("get_current_model")
                    request("set_expression", id="F01")
                    request("do_motion", group="Idle", index=0, priority=2)
                    request("set_mouth_y", value=.5)
                    request("set_look", x=.25, y=-.25)
                    request("set_model_zoom", value=1.2)
                    request("set_model_position", x=.1, y=-.1)
                    request("set_hover_opacity", enabled=True, opacity=0)
                    request("begin_transform_edit")
                    request("update_transform_edit", cropped=True, crop_x=10000, crop_y=10000,
                            crop_width=100, crop_height=100)
                    cropped = request("apply_transform_edit")
                    assert cropped["effective_width"] == 0 and cropped["effective_height"] == 0
                    request("get_status")  # Allow the empty cropped silhouette to be presented.
                    request("reset_transform_settings")
                    assert "model_region" not in json.loads((config / "config.json").read_text())
                    request("begin_transform_edit", screen=True)
                    assert request("get_transform_settings")["screen_editing"] is True
                    request("cancel_transform_edit")
                    request("toggle_hidden")
                    assert request("get_status")["hidden"] is True
                    request("toggle_hidden")
                    assert request("get_status")["hidden"] is False
                    request("release_source")
                    request("next_model")
                    request("get_hotkeys")
                    subprocess.run([sys.executable, str(Path(__file__).with_name("output_smoke.py"))],
                                   env=client_env, check=True, timeout=30)
                    model_log.flush()
                    assert "Mesa: error:" not in (logs / "model.log").read_text(), "OpenGL rendering error"
                    request("quit")
                assert app.wait(timeout=15) == 0, "Model exited with a failure status"
                model_log.flush()
                text = (logs / "model.log").read_text()
                assert_layer_shell(text)
                assert "GPU resources released before" in text, "Missing ordered GPU teardown"
                assert re.search(r"wl_region[@#]\d+\.add\(", text), "Real model produced no input silhouette"
                # SDK shader sets share program IDs and delete some twice at shutdown.
                # The same diagnostic occurs with the native backend; do not hide any
                # rendering error or expand this allowlist to unrelated GL failures.
                errors = [line for line in text.splitlines() if "Mesa: error:" in line]
                assert all(line == "Mesa: error: GL_INVALID_VALUE in glDeleteProgram" for line in errors), errors
                assert "Wayland protocol error" not in text
                if errors:
                    print("Known Cubism SDK teardown diagnostic: duplicate glDeleteProgram (also present in native baseline)")
                print(f"Qt two-output/model/IPC/teardown smoke passed (scale={args.scale}); logs: {logs}")
            finally:
                stop(app)
                stop(kwin)
    return 0


if __name__ == "__main__":
    sys.exit(main())
