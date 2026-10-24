import io
import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from PIL import Image

import ocr_probe


class ProbeTests(unittest.TestCase):
    def test_positive_limits(self):
        self.assertEqual(ocr_probe.positive_int("2"), 2)
        for value in ("0", "-1"):
            with self.assertRaises(argparse.ArgumentTypeError):
                ocr_probe.positive_int(value)

    def test_normalize(self):
        result = ocr_probe.normalize_result({
            "rec_texts": ["姐姐 OCR"], "rec_scores": [0.95],
            "rec_polys": [[[0, 1], [2, 1], [2, 3], [0, 3]]],
        })
        self.assertEqual(result[0]["text"], "姐姐 OCR")
        self.assertEqual(result[0]["polygon"][0], [0.0, 1.0])

    def test_empty_result(self):
        self.assertEqual(ocr_probe.normalize_result({
            "rec_texts": [], "rec_scores": [], "rec_polys": [],
        }), [])

    def test_mismatched_result(self):
        with self.assertRaises(ValueError):
            ocr_probe.normalize_result({
                "rec_texts": ["text"], "rec_scores": [], "rec_polys": [],
            })

    def test_missing_model_does_not_download(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "missing model file"):
                ocr_probe.model_directory(Path(directory), "missing")

    def test_jpeg_without_frame_count(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "sample.jpg"
            Image.new("RGB", (32, 24), "white").save(image)
            engine = Mock()
            engine.predict.return_value = [{"rec_texts": [], "rec_scores": [], "rec_polys": []}]
            with patch.object(ocr_probe, "memory", return_value={}):
                result = ocr_probe.recognize(engine, image, 4096)
            self.assertEqual((result["width"], result["height"]), (32, 24))
            engine.predict.assert_called_once_with(str(image.resolve()))

    def test_oversized_image_rejected_before_inference(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "large.png"
            Image.new("RGB", (33, 24), "white").save(image)
            engine = Mock()
            with self.assertRaisesRegex(ValueError, "exceeds --max-side"):
                ocr_probe.recognize(engine, image, 32)
            engine.predict.assert_not_called()

    def test_animated_image_rejected_before_inference(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "animated.gif"
            Image.new("RGB", (16, 16), "white").save(
                image, save_all=True, append_images=[Image.new("RGB", (16, 16), "black")]
            )
            engine = Mock()
            with self.assertRaisesRegex(ValueError, "single-frame"):
                ocr_probe.recognize(engine, image, 4096)
            engine.predict.assert_not_called()

    def test_server_recovers_after_invalid_request(self):
        requests = 'invalid\n[]\n{"id": 7}\n{"id": 8, "image": "sample.png"}\n'
        output = io.StringIO()
        with patch("sys.stdin", io.StringIO(requests)), patch("sys.stdout", output):
            with patch.object(ocr_probe, "recognize", return_value={"lines": []}):
                ocr_probe.serve(object(), 4096)
        replies = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([r["ok"] for r in replies], [False, False, False, True])
        self.assertEqual(replies[-1]["id"], 8)

    def test_server_recovers_after_inference_failure(self):
        requests = '{"id": 1, "image": "bad.png"}\n{"id": 2, "image": "good.png"}\n'
        output = io.StringIO()
        with patch("sys.stdin", io.StringIO(requests)), patch("sys.stdout", output):
            with patch.object(ocr_probe, "recognize", side_effect=[ValueError("bad image"), {"lines": []}]):
                ocr_probe.serve(object(), 4096)
        replies = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(replies[0], {"id": 1, "ok": False, "error": "bad image"})
        self.assertTrue(replies[1]["ok"])


if __name__ == "__main__":
    unittest.main()
