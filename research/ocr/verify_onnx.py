"""Compare converted ONNX tensors with Paddle on identical synthetic inputs."""

import argparse
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort
import paddle.inference as paddle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-root", type=Path, required=True)
    parser.add_argument("--onnx-root", type=Path, required=True)
    args = parser.parse_args()
    rng = np.random.default_rng(20261023)
    report = []
    for role, name, shapes in (
        ("detection", "PP-OCRv6_small_det", [(1, 3, 64, 96), (1, 3, 128, 64)]),
        ("recognition", "PP-OCRv6_medium_rec", [(1, 3, 48, 320), (1, 3, 48, 640), (2, 3, 48, 160)]),
    ):
        directory = args.models_root / name
        config = paddle.Config(str(directory / "inference.json"), str(directory / "inference.pdiparams"))
        config.disable_gpu()
        config.disable_mkldnn()
        config.set_cpu_math_library_num_threads(2)
        config.disable_glog_info()
        predictor = paddle.create_predictor(config)
        options = ort.SessionOptions()
        options.intra_op_num_threads = 2
        options.inter_op_num_threads = 1
        options.log_severity_level = 3
        session = ort.InferenceSession(str(args.onnx_root / f"{role}.onnx"), options, providers=["CPUExecutionProvider"])
        for shape in shapes:
            data = rng.uniform(-1, 1, shape).astype(np.float32)
            handle = predictor.get_input_handle(predictor.get_input_names()[0])
            handle.reshape(shape)
            handle.copy_from_cpu(data)
            predictor.run()
            expected = predictor.get_output_handle(predictor.get_output_names()[0]).copy_to_cpu()
            actual = session.run(None, {session.get_inputs()[0].name: data})[0]
            np.testing.assert_allclose(actual, expected, atol=1e-4, rtol=1e-3)
            report.append({"model": name, "input_shape": shape,
                           "max_absolute_error": float(np.max(np.abs(actual - expected)))})
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
