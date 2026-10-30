"""Native ONNX repository contract; set OCR_NATIVE_HELPER to a built helper.

Fixtures are tiny ONNX Constant graphs encoded with the standard library. No
Python ONNX/Paddle packages, network access, real weights, or model conversion.
"""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def integer(field, value):
    return varint(field << 3) + varint(value)


def data(field, value):
    if isinstance(value, str):
        value = value.encode()
    return varint((field << 3) | 2) + varint(len(value)) + value


def value_info(name, shape, dtype=1):
    dimensions = b"".join(data(1, integer(1, size) if size > 0 else data(2, f"dynamic_{i}"))
                          for i, size in enumerate(shape))
    tensor_type = integer(1, dtype) + data(2, dimensions)
    return data(1, name) + data(2, data(1, tensor_type))


def constant_model(input_shape, output_shape, values, declared_shape=None, dtype=1):
    """Encode ModelProto/GraphProto/Constant/TensorProto (ONNX IR 8, opset 13)."""
    tensor = b"".join(integer(1, size) for size in output_shape)
    tensor += integer(2, 1) + data(9, struct.pack(f"<{len(values)}f", *values))
    attribute = data(1, "value") + data(5, tensor) + integer(20, 4)
    node = data(2, "output") + data(4, "Constant") + data(5, attribute)
    graph = data(1, node) + data(2, "ocr-test")
    graph += data(11, value_info("input", input_shape, dtype))
    graph += data(12, value_info("output", declared_shape or output_shape))
    return integer(1, 8) + data(7, graph) + data(8, integer(2, 13))


DETECTION_YAML = """Global:
  model_name: PP-OCRv6_small_det
PreProcess:
  transform_ops:
  - DecodeImage: {channel_first: false, img_mode: BGR}
  - DetResizeForTest: null
  - NormalizeImage:
      mean: [0.485, 0.456, 0.406]
      std: [0.229, 0.224, 0.225]
      scale: 1./255.
      order: hwc
  - ToCHWImage: null
PostProcess:
  name: DBPostProcess
  thresh: 0.2
  box_thresh: 0.45
  unclip_ratio: 1.4
  max_candidates: 3000
"""

RECOGNITION_YAML = """Global:
  model_name: PP-OCRv6_small_rec
PreProcess:
  transform_ops:
  - DecodeImage: {channel_first: false, img_mode: BGR}
  - RecResizeImg: {image_shape: [3, 48, 320]}
PostProcess:
  name: CTCLabelDecode
  character_dict:
  - '中'
  - "'"
  - '\\'
"""


class NativeModelDirectories(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        helper = os.environ.get("OCR_NATIVE_HELPER")
        if not helper:
            raise unittest.SkipTest("Set OCR_NATIVE_HELPER to run native ONNX model contract tests")
        cls.helper = str(Path(helper).resolve())

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ocr-native-models-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.detection = self.root / "检测 model"
        self.recognition = self.root / "识别 model"
        for directory, metadata in ((self.detection, DETECTION_YAML), (self.recognition, RECOGNITION_YAML)):
            directory.mkdir()
            (directory / "inference.yml").write_text(metadata)
            # Official inference.json is a Paddle graph, not a dictionary.
            (directory / "inference.json").write_text('{"program": {}}')
        probabilities = [0.9 if 20 <= y < 40 and 5 <= x < 55 else 0.0 for y in range(64) for x in range(64)]
        (self.detection / "inference.onnx").write_bytes(constant_model([1, 3, -1, -1], [1, 1, 64, 64], probabilities))
        self.recognition_model()

    def recognition_model(self, classes=5, input_shape=None, dtype=1):
        values = [1.0 if column == index else 0.0 for index in (1, 1, 0, 2, classes - 1) for column in range(classes)]
        (self.recognition / "inference.onnx").write_bytes(constant_model(
            input_shape or [1, 3, 48, -1], [1, 5, classes], values, dtype=dtype))

    def start(self, requests="", extra=()):
        result = subprocess.run([self.helper, "--detection-model-dir", str(self.detection),
                                 "--recognition-model-dir", str(self.recognition), "--serve", *extra],
                                input=requests, capture_output=True, text=True, timeout=30)
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        return result, replies

    def rejected(self, code):
        result, replies = self.start()
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(replies), 1, result.stdout)
        self.assertEqual(replies[0]["event"], "error")
        self.assertEqual(replies[0]["protocol"], 1)
        self.assertEqual(replies[0]["code"], code, result.stdout)
        self.assertTrue(replies[0]["error"])
        return replies[0]

    def test_repository_metadata_ctc_and_no_writes(self):
        before = {str(path.relative_to(self.root)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in self.root.rglob("*") if path.is_file()}
        image = self.root / "image.ppm"
        image.write_bytes(b"P6\n64 64\n255\n" + b"\xff" * (64 * 64 * 3))
        request = json.dumps({"protocol": 1, "id": "fixture", "image": str(image)})
        result, replies = self.start("not-json\n" + request + "\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(replies[0]["event"], "ready")
        self.assertEqual(replies[0]["recognition_model"], "PP-OCRv6_small_rec")
        self.assertNotIn("profile", replies[0])
        self.assertFalse(replies[1]["ok"])
        self.assertTrue(replies[2]["ok"], replies[2])
        lines = replies[2]["result"]["lines"]
        self.assertEqual(len(lines), 1, replies[2])
        self.assertEqual(lines[0]["text"], "中' ")
        self.assertEqual(lines[0]["confidence"], 1)
        after = {str(path.relative_to(self.root)): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in self.root.rglob("*") if path.is_file() and path != image}
        self.assertEqual(before, after)

    def test_missing_metadata_identifies_directory(self):
        (self.recognition / "inference.yml").unlink()
        reply = self.rejected("model_missing")
        self.assertIn(str(self.recognition), reply["error"])

    def test_missing_model_identifies_directory(self):
        (self.detection / "inference.onnx").unlink()
        reply = self.rejected("model_missing")
        self.assertIn(str(self.detection), reply["error"])

    def test_invalid_yaml(self):
        (self.recognition / "inference.yml").write_text("Global: [unclosed")
        self.rejected("model_incompatible")

    def test_wrong_model_family(self):
        (self.recognition / "inference.yml").write_text(RECOGNITION_YAML.replace("small_rec", "medium_rec"))
        self.rejected("model_incompatible")

    def test_empty_dictionary(self):
        (self.recognition / "inference.yml").write_text(RECOGNITION_YAML.split("  character_dict:")[0] + "  character_dict: []\n")
        self.rejected("model_incompatible")

    def test_output_dictionary_mismatch(self):
        self.recognition_model(classes=6)
        self.rejected("model_incompatible")

    def test_probe_rejects_invalid_detector_output_before_ready(self):
        (self.detection / "inference.onnx").write_bytes(constant_model(
            [1, 3, -1, -1], [1, 1, 4097, 1], [0.0] * 4097))
        reply = self.rejected("model_incompatible")
        self.assertIn("validation failed", reply["error"])

    def test_unsupported_recognition_preprocessing(self):
        (self.recognition / "inference.yml").write_text(RECOGNITION_YAML.replace("[3, 48, 320]", "[3, 32, 320]"))
        self.rejected("model_incompatible")

    def test_unsupported_detector_normalization(self):
        (self.detection / "inference.yml").write_text(DETECTION_YAML.replace("0.485", "0.5"))
        self.rejected("model_incompatible")

    def test_invalid_detector_postprocessing(self):
        (self.detection / "inference.yml").write_text(DETECTION_YAML.replace("thresh: 0.2", "thresh: .nan"))
        self.rejected("model_incompatible")

    def test_incompatible_static_recognition_dimensions(self):
        for shape in ([1, 3, 32, -1], [1, 3, 48, 320], [2, 3, 48, -1]):
            with self.subTest(shape=shape):
                self.recognition_model(input_shape=shape)
                self.rejected("model_incompatible")

    def test_incompatible_input_type(self):
        self.recognition_model(dtype=6)  # int32 instead of float32
        self.rejected("model_incompatible")

    def test_unloadable_onnx(self):
        (self.detection / "inference.onnx").write_bytes(b"not an ONNX graph")
        self.rejected("model_load_failed")

    def test_invalid_cli_reports_error_not_ready(self):
        for extra in (("--models", "legacy"), ("--detection-model-dir",), ("--serve",)):
            with self.subTest(extra=extra):
                result, replies = self.start(extra=extra)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(len(replies), 1)
                self.assertEqual(replies[0]["event"], "error")
                self.assertEqual(replies[0]["code"], "invalid_arguments")


if __name__ == "__main__":
    unittest.main()
