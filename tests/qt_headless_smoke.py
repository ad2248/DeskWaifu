"""Opt-in Qt backend smoke test on a private two-output KWin and D-Bus session.

Usage: python3 tests/qt_headless_smoke.py BUILD_DIR SDK_ROOT [--scale 1.5]
Logs are retained in BUILD_DIR/qt-smoke-logs. No user's desktop/config is touched.
"""

import argparse
import json
import math
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
    # 协议声明和 QWindow 属性不足以证明窗口获得了正确角色。
    assert re.search(r"zwlr_layer_shell_v1[@#]\d+\.get_layer_surface\(", trace), \
        "Model window did not actually acquire a layer-shell role"
    assert not re.search(r"xdg_wm_base[@#]\d+\.get_xdg_surface\(", trace), \
        "Model window fell back to an ordinary xdg-shell window"


def assert_keyboard_capability(trace, expected):
    capabilities = re.findall(r"wl_seat[@#]\d+\.capabilities\((\d+)\)", trace)
    assert capabilities, "Missing Wayland seat capability evidence"
    assert any(int(value) & 2 for value in capabilities) == expected, \
        "Wayland keyboard capability does not match the focus test mode"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("--scale", default="1")
    parser.add_argument("--installed", action="store_true", help="install the application component into a private prefix before running")
    args = parser.parse_args()
    if os.environ.get("DESKWAIFU_TEST_ISOLATED") != "1":
        return subprocess.call([sys.executable, str(Path(__file__).resolve().parents[1] / "scripts/test-environment.py"),
                                sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]])
    for program in ("kwin_wayland", "dbus-run-session"):
        if not shutil.which(program):
            print(f"Missing {program}", file=sys.stderr)
            return 1
    if not os.environ.get("DESKWAIFU_QT_TEST_DBUS"):
        isolated_env = dict(os.environ, DESKWAIFU_QT_TEST_DBUS="1", DISPLAY="",
                            WAYLAND_DISPLAY="qt-model-test", QT_QPA_PLATFORMTHEME="generic",
                            QT_NO_XDG_DESKTOP_PORTAL="1")
        return subprocess.call(["dbus-run-session", "--", sys.executable,
                                str(Path(__file__).resolve()), *sys.argv[1:]], env=isolated_env)

    build = args.build.resolve()
    resources = args.sdk.resolve() / "Samples/Resources"
    assert (resources / "Haru/Haru.model3.json").is_file(), resources
    log_root = build / "qt-smoke-logs" / args.scale
    log_root.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix="run-", dir=log_root))
    temp_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="qt-model-", dir=temp_root) as directory:
        root = Path(directory)
        binaries = build / "bin"
        cli = Path(__file__).resolve().parents[1] / "deskwaifu-ctl"
        if args.installed:
            prefix = root / "prefix"
            with (logs / "install.log").open("w") as log:
                subprocess.run(["cmake", "--install", str(build), "--component", "DeskWaifu", "--prefix", str(prefix)],
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
            binaries = prefix / "bin"
            cli = binaries / "deskwaifu-ctl"
            assert (prefix / "share/applications/deskwaifu-control.desktop").is_file()
            for name in ("deskwaifu", "deskwaifu-control", "deskwaifu-ctl"):
                assert os.access(binaries / name, os.X_OK), name
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        config = root / "config/deskwaifu"
        config.mkdir(parents=True)
        aliases = root / "models"
        aliases.mkdir()
        unusual_names = ['夕颜"\\\n001', '001']
        for name in unusual_names:
            # 目录索引标识会解析真实目录；两个指向 Haru 的目录链接会合并为一个模型。
            # 独立覆盖目录可以保留各自别名。
            overlay = aliases / name
            overlay.mkdir()
            for item in (resources / "Haru").iterdir():
                (overlay / item.name).symlink_to(item, target_is_directory=item.is_dir())
        (config / "config.json").write_text(json.dumps({"default_model": "Haru", "additional_model_dirs": [str(aliases)]}))
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CACHE_HOME=str(root / "cache"), XDG_DATA_HOME=str(root / "data"),
                   LIBGL_ALWAYS_SOFTWARE="1", MESA_DEBUG="context",
                   QT_LOGGING_RULES="qt.qpa.wayland.warning=true")
        # 不继承真实合成器的控制套接字或 Qt shell 覆盖设置。
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
                    # KWin 的 --scale 控制虚拟模式尺寸，而非输出逻辑缩放。
                    # 应通过输出协议应用缩放。
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
                with (logs / "window.log").open("w") as log:
                    subprocess.run([str(build / "bin/qt-desktop-window-test")], env=client_env,
                                   stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
                window_log = (logs / "window.log").read_text()
                with (logs / "thread-transfer.log").open("w") as log:
                    subprocess.run([str(build / "bin/gpu-transfer-test")],
                                   env=dict(client_env, DESKWAIFU_TEST_RENDER_DEVICE="auto"),
                                    stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
                with (logs / "click-through.log").open("w") as log:
                    subprocess.run([str(build / "bin/click-through-test")], env=client_env,
                                   stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
                assert_layer_shell(window_log)
                assert_keyboard_capability(window_log, expected=True)
                assert "outputs=2" in window_log, window_log[-3000:]
                assert window_log.count("Output switch configured and committed") >= 2
                if "DESKWAIFU_QT_TEST_HOTPLUG" in client_env:
                    assert "hotplug=1" in window_log
                # 确保真实 Wayland 输入区域覆盖空区域和非空区域两种状态。
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
                assert empty and nonempty, "Qt did not submit both empty and shaped input regions"

                app = subprocess.Popen([str(binaries / "deskwaifu"), "--models_dir", str(resources)],
                                       env=client_env, cwd=root, stdout=model_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 20
                while not (runtime / "deskwaifu.sock").exists():
                    assert app.poll() is None, (logs / "model.log").read_text()[-6000:]
                    assert time.monotonic() < deadline, "Model startup timed out"
                    time.sleep(.05)
                # 整个编辑和流事务期间保持同一连接。
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(10)
                    connection.connect(str(runtime / "deskwaifu.sock"))
                    stream = connection.makefile("rb")

                    def request(command, **values):
                        connection.sendall((json.dumps(dict(command=command, **values)) + "\n").encode())
                        response = json.loads(stream.readline())
                        assert response["ok"], (command, response)
                        return response

                    # 通过套接字测试真实 AppCommandHandler，
                    # 不在测试中再实现一套命令解析。
                    def raw_request(payload):
                        connection.sendall(payload.encode() + b"\n")
                        return json.loads(stream.readline())

                    for payload in [
                        '{"metadata":{"command":"quit"},"command":"get_status"}',
                        '{"note":"command","value":"quit","command":"get_status"}',
                        '{"comm\\u0061nd":"get_status"}',
                    ]:
                        assert raw_request(payload)["ok"], payload
                    available = request("get_available_models")["models"]
                    assert all(name in available for name in unusual_names), available
                    for name in unusual_names:
                        request("set_model", name=name)
                        assert request("get_current_model")["model"] == name
                        result = subprocess.run([str(cli), "set_model", "--name", name], env=client_env,
                                                capture_output=True, text=True, timeout=10)
                        assert result.returncode == 0, result.stderr
                        assert request("get_current_model")["model"] == name
                    result = subprocess.run([str(cli), "set_model", "--name", "missing"], env=client_env,
                                            capture_output=True, text=True, timeout=10)
                    assert result.returncode != 0 and json.loads(result.stdout)["ok"] is False
                    request("set_model", name="Haru")
                    if args.installed:
                        with (logs / "installed-control.log").open("w") as log:
                            subprocess.run([str(binaries / "deskwaifu-control"), "--smoke-test"],
                                           env=client_env, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
                    request("set_mouth_y", value=.4)
                    request("set_look", x=.2, y=.1)
                    request("set_model", name="Haru")
                    controls = request("get_control_status")
                    assert math.isclose(controls["mouth"], .4, abs_tol=1e-6), controls
                    assert raw_request('{"command":"set_model","index":-1}')["ok"] is False
                    assert request("get_control_status")["has_mouth"] is True
                    request("next_model")
                    controls = request("get_control_status")
                    assert not controls["has_mouth"] and not controls["has_look"], controls
                    request("set_model", name="Haru")
                    before = request("get_status")
                    for index in ['"banana"', '0.9', '2147483648', '-2147483649',
                                  '1e100', 'null', 'true', '"0"', '"0junk"', '[]', '{}']:
                        reply = raw_request('{"command":"set_model","index":' + index + '}')
                        assert reply["ok"] is False, (index, reply)
                        assert request("get_status")["model_index"] == before["model_index"]
                    for payload in ['{"command":"set_model","name":"missing","index":0}',
                                    '{"command":"set_model","index":0,"stream_source":"invalid"}',
                                    '{"command":true}', '{"command":"quit","command":"get_status"}',
                                    '{"command":"get_status"} trailing',
                                    '{"command":"get_status"}//comment',
                                    '{"command":"set_look","x":"bad"}']:
                        assert raw_request(payload)["ok"] is False, payload
                    for revision in [-1, -18446744073709551615, "0", .5, 4294967295]:
                        payload = json.dumps(dict(command="begin_transform_edit", expected_revision=revision))
                        assert raw_request(payload)["ok"] is False, revision
                        assert request("get_transform_settings")["editing"] is False
                    assert raw_request('{"command":"set_expression","id":"\\u0046\\u0030\\u0031"}')["ok"]
                    for value in ['true', '"true"', '1', '"1"']:
                        assert raw_request('{"command":"set_look","reset":' + value + '}')["ok"]
                    request("get_status")
                    request("get_current_model")
                    assert request("get_mouse_look")["radius"] == .5
                    assert request("set_mouse_look", radius=.25)["radius"] == .25
                    assert json.loads((config / "config.json").read_text())["mouse_look_radius"] == .25
                    assert request("get_mouse_look")["radius"] == .25
                    assert request("set_mouse_look", radius=0)["radius"] == 0
                    assert request("set_mouse_look", radius=.5)["radius"] == .5
                    # 配置写入会改变共享配置快照；应显式刷新，
                    # 避免与编辑器的定期重新加载发生竞争。
                    request("reload_transform_settings")
                    request("set_expression", id="F01")
                    request("do_motion", group="Idle", index=0, priority=2)
                    request("set_mouth_y", value=.5)
                    request("set_look", x=.25, y=-.25)
                    request("set_model_zoom", value=1.2)
                    request("set_model_position", x=.1, y=-.1)
                    request("set_hover_opacity", enabled=True, opacity=0)
                    request("begin_transform_edit")
                    for values in [{"crop_x": .5}, {"crop_width": 2147483648}, {"x": "0.5"}]:
                        reply = raw_request(json.dumps(dict(command="update_transform_edit", **values)))
                        assert reply["ok"] is False, (values, reply)
                    request("update_transform_edit", cropped=True, crop_x=10000, crop_y=10000,
                            crop_width=100, crop_height=100)
                    cropped = request("apply_transform_edit")
                    assert cropped["effective_width"] == 0 and cropped["effective_height"] == 0
                    request("get_status")  # 给空裁剪轮廓一次呈现机会。
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
                    # 只发送流水请求、不读取回复的客户端不能独占命令派发，
                    # 也不能阻止实际 Wayland 帧提交。
                    with socket.socket(socket.AF_UNIX) as pressure:
                        pressure.settimeout(5)
                        pressure.connect(str(runtime / "deskwaifu.sock"))
                        commits_before = len(re.findall(r"wl_surface[@#]\d+\.commit\(",
                                                        (logs / "model.log").read_text()))
                        pressure.sendall(b'{"command":"get_status"}\n' * 256)
                        for _ in range(16):
                            request("get_status")
                        # 就绪驱动的 IPC 可能在下一次呈现前完成 16 个回复。
                        # 应在实际时限内观察帧提交，
                        # 而非依赖与旧帧轮询绑定的请求次数。
                        deadline = time.monotonic() + 3
                        while True:
                            commits_after = len(re.findall(r"wl_surface[@#]\d+\.commit\(",
                                                           (logs / "model.log").read_text()))
                            if commits_after > commits_before:
                                break
                            assert time.monotonic() < deadline, "IPC pressure stopped frame commits"
                            request("get_status")
                            time.sleep(.01)
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
                # SDK 着色器组共享程序 ID，关闭时会重复删除部分程序。
                # 不能隐藏渲染错误，也不能将无关 GL 错误加入允许列表。
                # 此诊断在 Qt 窗口迁移前就已存在。
                errors = [line for line in text.splitlines() if "Mesa: error:" in line]
                assert all(line == "Mesa: error: GL_INVALID_VALUE in glDeleteProgram" for line in errors), errors
                assert "Wayland protocol error" not in text
                if errors:
                    print("Known Cubism SDK teardown diagnostic: duplicate glDeleteProgram")
                print(f"Qt two-output/model/IPC/teardown smoke passed (scale={args.scale}); logs: {logs}")
            finally:
                stop(app)
                stop(kwin)
    return 0


if __name__ == "__main__":
    sys.exit(main())
