"""Exercise display selection through the real IPC and Wayland/EGL backend."""
import json
import os
from pathlib import Path
import socket

with socket.socket(socket.AF_UNIX) as client:
    client.settimeout(10)
    client.connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], "deskwaifu.sock"))
    stream = client.makefile("rb")

    def request(command, **values):
        client.sendall((json.dumps(dict(command=command, **values)) + "\n").encode())
        return json.loads(stream.readline())

    initial = request("get_transform_settings")
    assert initial["ok"] and len(initial["outputs"]) >= 2, initial
    original = next(o["name"] for o in initial["outputs"] if o["current"])
    target = next(o["name"] for o in initial["outputs"] if not o["current"])
    assert not request("set_output", name="nonexistent-output")["ok"]
    assert request("begin_transform_edit")["ok"]
    assert not request("set_output", name=target)["ok"]
    assert request("update_transform_edit", x=2, y=-1, zoom=1.5, cropped=True,
                   crop_x=10, crop_y=20, crop_width=300, crop_height=400)["ok"]
    assert request("apply_transform_edit")["ok"]
    for name in (target, original):
        assert request("set_output", name=name)["ok"]
        state = request("get_transform_settings")
        assert state["x"] == state["y"] == 0 and state["zoom"] == 1.5, state
        assert not state["cropped"], state
        assert next(o["name"] for o in state["outputs"] if o["current"]) == name, state
        saved = json.loads(Path(state["config_path"]).read_text())
        assert saved["model_output"] == name, saved
        assert saved["model_x"] == saved["model_y"] == 0, saved
        assert saved["model_scale"] == 1.5 and "model_region" not in saved, saved
print("Display selection, centering, crop clearing and persistence passed")
