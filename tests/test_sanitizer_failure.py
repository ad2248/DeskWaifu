"""Run the instrumented fixture as an ordinary CTest failure, not WILL_FAIL."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="ubsan-gate-") as directory:
    source = Path(directory)
    binary = Path(sys.argv[1]).resolve()
    (source / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(fixture NONE)\nenable_testing()\n'
        f'add_test(NAME ub-runtime COMMAND [=[{binary}]=])\n')
    build = source / "build"
    subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=True)
    result = subprocess.run([sys.executable, str(root / "scripts/run-test-gate.py"), str(build),
                             "--test", "^ub-runtime$"], capture_output=True, text=True, timeout=15)
    print(result.stdout)
    print(result.stderr, file=sys.stderr)
    assert result.returncode != 0, "UBSan diagnostic must fail the ordinary gate"
    assert "runtime error: signed integer overflow" in result.stdout, "preserve the actual UBSan diagnostic"
    report = json.loads(next((build / "test-logs").glob("ctest-*/summary.json")).read_text())
    assert report["tests"][0]["process_status"] == "failed", report
