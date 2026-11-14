"""独立环境中的真实 IPC 插件实例不能争用端点或相互清理。"""
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]


def query(instance):
    process, snapshot = instance
    assert process.poll() is None, "another instance stopped this platform"
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(3)
        client.connect(str(Path(snapshot["XDG_RUNTIME_DIR"]) / "deskwaifu.sock"))
        pid, _, _ = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        assert pid == snapshot["pid"], "IPC reached the wrong instance"
        client.sendall(b'{"command":"unknown_command"}\n')
        with client.makefile() as stream:
            assert json.loads(stream.readline()) == {"ok": False, "error": "no provider"}


def stop(process):
    if process.poll() is None:
        process.terminate()
    try:
        output, error = process.communicate(timeout=7)
    except subprocess.TimeoutExpired:
        process.kill()
        output, error = process.communicate()
        raise
    sys.stderr.write(error.decode(errors="replace"))


def main():
    executable, module = sys.argv[1:]
    bootstrap = '''import json, os, sys
from pathlib import Path
keys = ("HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR", "DBUS_SESSION_BUS_ADDRESS")
snapshot = {key: os.environ[key] for key in keys}
snapshot["pid"] = os.getpid()
Path(sys.argv[1]).write_text(json.dumps(snapshot))
(Path(snapshot["HOME"]) / "plugins").symlink_to(sys.argv[3], target_is_directory=True)
config = Path(snapshot["XDG_CONFIG_HOME"]) / "deskwaifu"
config.mkdir()
(config / "config.json").write_text(json.dumps({"plugin_dir": "~/plugins"}))
os.execv(sys.argv[2], [sys.argv[2]])
'''
    processes, instances = [], []
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        plugins = root / "plugins" / "ipc"
        plugins.mkdir(parents=True)
        shutil.copyfile(module, plugins / Path(module).name)
        try:
            for index in range(2):
                report = root / f"instance-{index}.json"
                process = subprocess.Popen(
                    [sys.executable, str(ROOT / "scripts/test-environment.py"), sys.executable,
                     "-c", bootstrap, str(report), executable, str(plugins.parent)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                processes.append(process)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and process.poll() is None:
                    if report.exists():
                        try:
                            snapshot = json.loads(report.read_text())
                        except json.JSONDecodeError:
                            continue
                        if (Path(snapshot["XDG_RUNTIME_DIR"]) / "deskwaifu.sock").exists():
                            break
                    time.sleep(.02)
                else:
                    raise AssertionError("independent platform did not publish its IPC endpoint")
                instances.append((process, snapshot))
            for key in instances[0][1]:
                assert instances[0][1][key] != instances[1][1][key], f"shared {key}"
            for instance in instances:
                query(instance)
            stop(processes[0])
            processes.pop(0)
            assert not Path(instances[0][1]["XDG_RUNTIME_DIR"]).exists(), "stopped runtime leaked"
            query(instances[1])
        finally:
            for process in processes:
                stop(process)
        for _, snapshot in instances:
            assert not Path(snapshot["HOME"]).exists(), "instance HOME leaked"
    return 0


if __name__ == "__main__":
    sys.exit(main())
