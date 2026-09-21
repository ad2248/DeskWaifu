"""Headless IPC regression using SDK assets and a small synthetic VTube configuration."""

import json
import os
from pathlib import Path
import shutil
import socket
import sys
import time


def prepare(sample):
    model = Path(os.environ["XDG_CONFIG_HOME"]) / "deskwaifu/models/HotkeyFixture"
    shutil.copytree(sample, model.parent / "Haru", copy_function=os.symlink)
    shutil.copytree(sample, model, copy_function=os.symlink)
    (model / "exp").mkdir()
    for name, parameter in (("watermark", "ParamAngleX"), ("blush", "ParamAngleY")):
        (model / f"exp/{name}.exp3.json").write_text(json.dumps({
            "Type": "Live2D Expression",
            "Parameters": [{"Id": parameter, "Value": 1, "Blend": "Add"}],
        }))
    (model / "Haru.vtube.json").write_text(json.dumps({"Hotkeys": [
        {"HotkeyID": name, "Action": "ToggleExpression", "File": f"{name}.exp3.json",
         "IsActive": True, "IsGlobal": True, "Triggers": {"Trigger1": key},
         "FadeSecondsAmount": 0.5}
        for name, key in (("watermark", "Tab"), ("blush", "N1"))
    ]}))


def request(command, **parameters):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(str(Path(os.environ["XDG_RUNTIME_DIR"]) / "deskwaifu.sock"))
        client.sendall(json.dumps({"command": command, **parameters}).encode() + b"\n")
        return json.loads(client.makefile("rb").readline())


def check():
    assert request("set_model", name="HotkeyFixture")["ok"]
    expressions = request("get_expressions")
    assert "watermark.exp3.json" in expressions["expressions"], expressions
    response = request("get_hotkeys")
    assert response["ok"] and len(response["hotkeys"]) == 2, response
    token = response["model_token"]

    def toggle(name):
        response = request("trigger_hotkey", id=name, model_token=token)
        assert response["ok"], response
        return {entry["id"]: entry["active"] for entry in response["hotkeys"]}

    assert not request("trigger_hotkey", id="unknown", model_token=token)["ok"]
    assert not request("trigger_hotkey", id="watermark", model_token="stale")["ok"]
    assert toggle("watermark") == {"watermark": True, "blush": False}
    assert toggle("blush") == {"watermark": True, "blush": True}
    assert request("set_expression", id="F01")["ok"]
    time.sleep(6)  # Persistent switches must survive the ordinary emotion timeout.
    assert all(entry["active"] for entry in request("get_hotkeys")["hotkeys"])
    assert toggle("watermark") == {"watermark": False, "blush": True}
    assert request("set_model", name="Haru")["ok"]
    assert request("get_hotkeys")["hotkeys"] == []
    assert not request("trigger_hotkey", id="watermark", model_token=token)["ok"]
    assert request("set_model", name="HotkeyFixture")["ok"]
    response = request("get_hotkeys")
    assert response["model_token"] != token
    assert not any(entry["active"] for entry in response["hotkeys"])
    print("Hotkey IPC discovery, persistence, independent toggles and stale-model checks passed")


if __name__ == "__main__":
    if sys.argv[1] == "prepare":
        prepare(sys.argv[2])
    else:
        check()
