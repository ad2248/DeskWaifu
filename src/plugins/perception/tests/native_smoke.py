"""Manual native-helper checks. Requires generated samples, not Paddle or Python OCR deps."""

import argparse
import json
from pathlib import Path
import resource
import subprocess
import tempfile
import time
import unicodedata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--detection-model-dir", type=Path,
                        default=Path("/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det"))
    parser.add_argument("--recognition-model-dir", type=Path,
                        default=Path("/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec"))
    args = parser.parse_args()
    args.samples = args.samples.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    expected = json.loads((args.samples / "expected.json").read_text())
    requests = ["not-json", json.dumps({"protocol": 2, "id": "bad-version", "image": "unused"}),
                "x" * (64 * 1024 + 1)]
    for name in ("missing.png", "oversized.png", "animated.gif"):
        requests.append(json.dumps({"protocol": 1, "id": name, "image": str(args.samples / name)}))
    for name in expected:
        for repeat in range(3):
            requests.append(json.dumps({"protocol": 1, "id": f"{name}-{repeat}", "image": str(args.samples / name)}))
    requests.append(json.dumps({"protocol": 1, "id": "jpeg", "image": str(args.samples / "light.jpg")}))
    start = time.perf_counter()
    # 辅助进程不继承 Python 环境、模型缓存、GPU 配置或 LD_LIBRARY_PATH。
    env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
    command = [str(args.helper.resolve()), "--serve", "--detection-model-dir", str(args.detection_model_dir.resolve()),
               "--recognition-model-dir", str(args.recognition_model_dir.resolve())]
    result = subprocess.run(command, env=env,
                            input="\n".join(requests) + "\n", capture_output=True, text=True, timeout=180)
    (args.output / "native.jsonl").write_text(result.stdout)
    (args.output / "native.log").write_text(result.stderr)
    assert result.returncode == 0, result.stderr
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(replies) == len(requests) + 1
    assert replies[0]["event"] == "ready" and replies[0]["protocol"] == 1
    assert all(not reply["ok"] for reply in replies[1:7]), replies[1:7]
    # JPEG may change full-width punctuation; compare NFKC text without whitespace,
    # not byte-for-byte accuracy or a general recognition benchmark.
    normalize = lambda text: "".join(unicodedata.normalize("NFKC", text).split())
    timings = {}
    for reply in replies[7:]:
        assert reply["protocol"] == 1 and reply["ok"], reply
        value = reply["result"]
        name = reply["id"].rsplit("-", 1)[0]
        target = expected["light.png" if name == "jpeg" else name]
        assert sorted(normalize(line["text"]) for line in value["lines"]) == sorted(map(normalize, target)), reply
        for line in value["lines"]:
            assert 0 <= line["confidence"] <= 1
            assert len(line["polygon"]) == 4
            assert all(0 <= x < value["width"] and 0 <= y < value["height"] for x, y in line["polygon"])
        timings.setdefault(name, []).append(value["elapsed_s"])
    summary = {"checks": "passed", "text_comparison": "NFKC, ignoring whitespace", "timings_s": timings,
               "total_process_s": time.perf_counter() - start,
               "child_peak_rss_mib": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss / 1024}
    with tempfile.TemporaryDirectory(prefix="bad-recognition-metadata-", dir=args.output) as temporary:
        invalid = Path(temporary)
        (invalid / "inference.onnx").symlink_to(args.recognition_model_dir.resolve() / "inference.onnx")
        metadata = (args.recognition_model_dir / "inference.yml").read_text()
        assert "  character_dict:" in metadata
        (invalid / "inference.yml").write_text(metadata.replace(
            "  character_dict:", "  character_dict: []\n  original_dictionary:", 1))
        corrupted = subprocess.run([str(args.helper.resolve()), "--serve",
                                   "--detection-model-dir", str(args.detection_model_dir.resolve()),
                                   "--recognition-model-dir", str(invalid.resolve())],
                                   env=env, capture_output=True, text=True, timeout=10)
        assert corrupted.returncode != 0
        error = json.loads(corrupted.stdout)
        assert error["event"] == "error" and error["code"] == "model_incompatible", error
        assert "dictionary" in error["error"], error
    summary["invalid_dictionary_rejected_before_ready"] = True
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
