"""Local-only CPU OCR experiment; no desktop capture or application integration."""

import argparse
import contextlib
import json
import os
from pathlib import Path
import resource
import sys
import time


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def model_directory(root, name):
    directory = root / name
    for filename in ("inference.json", "inference.pdiparams", "inference.yml"):
        if not (directory / filename).is_file():
            raise ValueError(f"missing model file: {directory / filename}")
    return directory


def normalize_result(result):
    texts = result["rec_texts"]
    scores = result["rec_scores"]
    polygons = result["rec_polys"]
    if not len(texts) == len(scores) == len(polygons):
        raise ValueError("OCR returned inconsistent text/score/polygon counts")
    return [
        {"text": text, "confidence": float(score),
         "polygon": [[float(x), float(y)] for x, y in polygon]}
        for text, score, polygon in zip(texts, scores, polygons)
    ]


def memory():
    import psutil

    return {
        "rss_mib": round(psutil.Process().memory_info().rss / 1024**2, 2),
    # Linux 的 ru_maxrss 单位为 KiB，包含导入、加载和之前请求占用的内存。
        "process_peak_rss_mib": round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024, 2),
    }


def load_engine(args):
    det_name = f"PP-OCRv6_{args.detector}_det"
    rec_name = "PP-OCRv6_medium_rec"
    det_dir = model_directory(args.models_root, det_name)
    rec_dir = model_directory(args.models_root, rec_name)
    # 跳过模型源连通性检查；显式本地路径可防止下载模型。
    os.environ["PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK"] = "True"
    start = time.perf_counter()
    with contextlib.redirect_stdout(sys.stderr):
        from paddleocr import PaddleOCR

        engine = PaddleOCR(
            text_detection_model_name=det_name,
            text_detection_model_dir=str(det_dir),
            text_recognition_model_name=rec_name,
            text_recognition_model_dir=str(rec_dir),
            use_doc_orientation_classify=False,
            use_doc_unwarping=False,
            use_textline_orientation=False,
            device="cpu",
            cpu_threads=args.threads,
            enable_mkldnn=False,
        )
    return engine, round(time.perf_counter() - start, 4)


def recognize(engine, image, max_side):
    from PIL import Image

    image = Path(image).expanduser().resolve(strict=True)
    # 拒绝超限图像而非缩放，使输出多边形保持原图像素坐标。
    with Image.open(image) as source:
        width, height = source.size
        if max(width, height) > max_side:
            raise ValueError(f"image exceeds --max-side {max_side}: {width}x{height}")
        if getattr(source, "n_frames", 1) != 1:
            raise ValueError("only single-frame images are supported")
    start = time.perf_counter()
    with contextlib.redirect_stdout(sys.stderr):
        results = list(engine.predict(str(image)))
    if len(results) != 1:
        raise ValueError("expected one OCR result")
    return {
        "image": str(image), "width": width, "height": height,
        "elapsed_s": round(time.perf_counter() - start, 4),
        "lines": normalize_result(results[0]), **memory(),
    }


def emit(payload):
    print(json.dumps(payload, ensure_ascii=False, allow_nan=False), flush=True)


def serve(engine, max_side):
            # 刻意串行处理请求，并发数限制为一，不使用后台队列。
    for line in sys.stdin:
        request_id = None
        try:
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError("request must be an object")
            request_id = request.get("id")
            if not isinstance(request.get("image"), str):
                raise ValueError("image must be a file path string")
            result = recognize(engine, request["image"], max_side)
            emit({"id": request_id, "ok": True, "result": result})
        except Exception as error:
            emit({"id": request_id, "ok": False, "error": str(error)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-root", type=Path, required=True)
    parser.add_argument("--detector", choices=("small", "medium"), default="small")
    parser.add_argument("--threads", type=positive_int, default=2)
    parser.add_argument("--max-side", type=positive_int, default=4096)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--image", type=Path)
    mode.add_argument("--serve", action="store_true")
    parser.add_argument("--repeat", type=positive_int, default=3)
    args = parser.parse_args()
    args.models_root = args.models_root.expanduser().resolve()
    try:
        engine, load_s = load_engine(args)
        if args.serve:
            emit({"event": "ready", "load_s": load_s, **memory()})
            serve(engine, args.max_side)
        else:
            runs = [recognize(engine, args.image, args.max_side) for _ in range(args.repeat)]
            emit({"detector": args.detector, "threads": args.threads,
                  "load_s": load_s, "runs": runs})
    except Exception as error:
        emit({"ok": False, "error": str(error)})
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
