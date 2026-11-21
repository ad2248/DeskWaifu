"""Exercise the read-only legacy Live2D config migration CLI."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().with_name("migrate-live2d-config.py")


class MigrationTest(unittest.TestCase):
    def invoke(self, document, *options):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "config.json"
            target = directory / "config.jsonc"
            source.write_text(document, encoding="utf-8")
            existing = b'{ // preserve user settings\n "terminal": {"opacity": 0.75}\n}\n'
            target.write_bytes(existing)
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(source), *options],
                cwd=directory, capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(source.read_text(encoding="utf-8"), document)
            self.assertEqual(target.read_bytes(), existing)
            self.assertEqual(set(directory.iterdir()), {source, target})
            return result

    def test_supported_fields_and_crop_preserve_effect(self):
        result = self.invoke(r'''{
          // Keep this input intact and preserve quoted comment markers.
          "idle_opacity": 0, "hover_opacity_enabled": true, "hover_opacity": 0.7,
          "mouse_look_radius": 0.25, "model_output": "DP-2", "render_device": "0000:44:1f.7",
          "additional_model_dirs": ["/mnt/模型//quoted\"name"],
          "model_x": 0.4, "model_y": -0.2, "model_scale": 2,
          "model_region": {"x": -23, "y": 67, "width": 1040, "height": 450},
          "window_width": 1900, "window_height": 1000
        }''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"terminal": {
            "opacity": 0, "hover_opacity": 0.7, "mouse_look_radius": 0.25,
            "output": "DP-2", "gpu": "0000:44:1f.7", "model_directory": '/mnt/模型//quoted"name',
            "transform": {
                "x": 0.0, "y": 0.0, "zoom": 1.0,
                "crop": {"x": -23, "y": 67, "width": 1040, "height": 450},
            },
        }})
        for field in ("model_x", "model_region", "window_width", "window_height", "primary directory"):
            self.assertIn(field, result.stderr)

    def test_crop_defaults_origins_and_uses_archived_integer_limits(self):
        result = self.invoke('{"model_region":{"width":2147483647,"height":1}}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["terminal"]["transform"]["crop"], {
            "x": 0, "y": 0, "width": 2147483647, "height": 1,
        })

    def test_hover_enable_and_archive_default(self):
        for document, expected in [
            ('{"hover_opacity":0.4}', None),
            ('{"hover_opacity_enabled":false}', None),
            ('{"hover_opacity_enabled":true}', 0.5),
            ('{"hover_opacity_enabled":true,"hover_opacity":0}', 0),
        ]:
            with self.subTest(document=document):
                result = self.invoke(document)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), {"terminal": {"hover_opacity": expected}})

    def test_ambiguous_models_and_other_unsupported_settings_are_reported(self):
        source = json.dumps({
            "additional_model_dirs": ["/first", "~/another"], "default_model": "Haru",
            "model_scale": 1.2, "emotion_timeout": 3, "plugin_dir": "/plugins",
            "fullscreen_opacity_enabled": True, "fullscreen_opacity": 0.2,
            "unrelated_plugin": {"token": "secret"},
        })
        result = self.invoke(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"terminal": {"emotion_timeout": 3}})
        for field in ("additional_model_dirs", "default_model", "model_scale",
                      "plugin_dir", "fullscreen_opacity_enabled", "fullscreen_opacity", "unrelated_plugin"):
            self.assertIn(field, result.stderr)
        self.assertNotIn("secret", result.stderr)

        result = self.invoke(source, "--model-directory", "/new/catalog",
                             "--startup-model", "/new/catalog/Haru/Haru.model3.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"terminal": {
            "model_directory": "/new/catalog", "startup_model": "/new/catalog/Haru/Haru.model3.json",
            "emotion_timeout": 3,
        }})

    def test_negative_emotion_timeout_disables_expiry(self):
        # 旧版把任意负值视为关闭，迁移到新版的标准哨兵值。
        result = self.invoke('{"emotion_timeout":-3}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"terminal": {"emotion_timeout": -1}})

    def test_reports_effective_radius_limit(self):
        result = self.invoke('{"mouse_look_radius":1.5}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"terminal": {"mouse_look_radius": 1.5}})
        self.assertIn("caps", result.stderr)

    def test_rejects_malformed_input_without_partial_proposal(self):
        cases = [
            ('[1]', "must be an object"),
            ('{} {}', "Extra data"),
            ('{"model_x": 0,}', "line 1 column"),
            (r'{"model_x":0,"model_\u0078":1}', "duplicate key"),
            ('{"model_region":{"x":0,"x":1}}', "duplicate key"),
            ('{"model_scale":1e400}', "non-finite"),
            ('{"model_scale":NaN}', "non-finite"),
            ('{"idle_opacity":true}', "idle_opacity"),
            ('{"idle_opacity":1.1}', "idle_opacity"),
            ('{"hover_opacity_enabled":1}', "hover_opacity_enabled"),
            ('{"mouse_look_radius":-0.01}', "mouse_look_radius"),
            ('{"model_scale":0}', "model_scale"),
            ('{"model_y":"0"}', "model_y"),
            ('{"model_region":{"width":1}}', "model_region.height"),
            ('{"model_region":{"height":1,"width":0}}', "model_region.width"),
            ('{"model_region":{"x":2147483648,"height":1,"width":1}}', "model_region.x"),
            ('{"model_region":{"x":0.25,"height":1,"width":1}}', "model_region.x"),
            ('{"model_region":{"height":1,"width":1,"w":1}}', "model_region"),
            ('{"render_device":"0000:00:20.0"}', "render_device"),
            (r'{"model_output":"DP-1\u0000"}', "model_output"),
            (r'{"model_output":"DP-1\u0085"}', "control characters"),
            (json.dumps({"model_output": "é" * 129}), "256 UTF-8 bytes"),
            (r'{"default_model":"\ud800"}', "surrogates"),
            ('{"additional_model_dirs":[5]}', "additional_model_dirs"),
            ('{"window_width":10.5}', "window_width"),
            ('{"terminal":{}}', "already contains a terminal section"),
        ]
        for document, expected in cases:
            with self.subTest(document=document):
                result = self.invoke(document)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertIn(expected, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_rejects_bad_model_overrides(self):
        for option, value in [("--model-directory", "relative"), ("--startup-model", "/not-a-model")]:
            with self.subTest(option=option):
                result = self.invoke("{}", option, value)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertIn(option, result.stderr)

    def test_rejects_fifo_without_hanging(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "legacy-config"
            os.mkfifo(path)
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(path)], capture_output=True, text=True, timeout=3,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("regular file", result.stderr)


if __name__ == "__main__":
    unittest.main()
