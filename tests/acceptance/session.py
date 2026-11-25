"""在私有无头 Plasma 会话中运行一个用例，并把会话日志写入用例目录。"""
import os
import subprocess
import sys
from pathlib import Path

LAUNCHER = Path(__file__).resolve().parents[2] / "scripts" / "with-headless-plasma.sh"


def run(case, suite, output, bin_dir):
    command = [
        "bash", str(LAUNCHER), sys.executable, "-B", str(suite),
        "--case", case.id, "--output", str(output), "--in-session",
    ]
    environment = os.environ | {
        "DESKWAIFU_ACCEPTANCE_BIN_DIR": str(bin_dir),
        "KWIN_SCREENSHOT_NO_PERMISSION_CHECK": "1",
        "QT_LOGGING_RULES": "kwin_scripting.debug=true",
    }
    with (Path(output) / case.title / "session.log").open("wb") as log:
        return subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=environment).returncode
