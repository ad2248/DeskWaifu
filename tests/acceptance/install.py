"""安装构建产出的软件包，并记录安装过程与落地事实。"""
import hashlib
import subprocess
from pathlib import Path

from record import STATUS_FAILED, Step, write_json

DEFAULT_BIN_DIR = Path("/usr/bin")
INSTALLED_FILES = (
    "/usr/bin/deskwaifu",
    "/usr/bin/deskwaifucli",
    "/usr/bin/deskwaifu-terminal-panel",
    "/usr/lib/deskwaifu/deskwaifu",
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(argv, check=True):
    result = subprocess.run(argv, capture_output=True, text=True)
    if check:
        result.check_returncode()
    return result


def find_package(directory):
    packages = sorted(Path(directory).glob("*.pkg.tar.zst"))
    if len(packages) != 1:
        raise RuntimeError(f"期望恰好一个 .pkg.tar.zst，实际 {len(packages)} 个: {directory}")
    return packages[0]


def package_depends(package):
    info = subprocess.check_output(["bsdtar", "-xOf", str(package), ".PKGINFO"], text=True)
    return [line.split("=", 1)[1].strip() for line in info.splitlines() if line.startswith("depend = ")]


def missing_dependencies(depends):
    # pacman -T 的退出码约定与常规命令相反，只按输出判定。
    result = run(["pacman", "-T", *depends], check=False)
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def collect(step):
    """记录安装后的包信息、文件清单、链接与摘要；只记录，不判定。"""
    for argv, name in (
        (["pacman", "-Q", "--info", "deskwaifu-bin"], "installed-info.log"),
        (["pacman", "-Ql", "deskwaifu-bin"], "installed-files.log"),
        (["readlink", "-f", "/usr/bin/deskwaifu"], "links.log"),
        (["/usr/bin/deskwaifucli", "--help"], "cli-help.log"),
    ):
        result = run(argv, check=False)
        step.log(name, result.stdout + result.stderr)
    installed = [path for path in INSTALLED_FILES if Path(path).exists()]
    installed += [str(path) for path in sorted(Path("/usr/lib/deskwaifu/plugins").glob("*.so"))]
    step.log("installed-sha256.log", run(["sha256sum", *installed]).stdout)


EXPECTED = "deskwaifu-bin 安装成功且运行依赖已满足"


def install(directory, package_dir):
    """安装目录中的软件包，返回已安装可执行文件所在目录。"""
    directory = Path(directory)
    directory.mkdir()
    try:
        package = find_package(package_dir)
    except Exception as error:
        step_dir = directory / "安装"
        step_dir.mkdir()
        write_json(step_dir / "result.json", {
            "order": 1,
            "name": "安装",
            "input": {"package_dir": str(package_dir)},
            "expected": EXPECTED,
            "status": STATUS_FAILED,
            "error": str(error),
        })
        raise
    with Step(
        directory,
        1,
        "安装",
        EXPECTED,
        {"package": package.name, "sha256": digest(package)},
    ) as step:
        depends = package_depends(package)
        missing = missing_dependencies(depends)
        step.log(
            "dependencies.log",
            "depend = " + " ".join(depends) + "\n"
            + ("missing = " + " ".join(missing) + "\n" if missing else "all dependencies satisfied\n"),
        )
        if missing:
            raise RuntimeError(f"缺少运行依赖: {' '.join(missing)}")
        result = run(["pacman", "-U", "--noconfirm", str(package)], check=False)
        step.log("pacman.log", result.stdout + result.stderr)
        result.check_returncode()
        collect(step)
        step.output = {"package": package.name, "version": run(["pacman", "-Q", "deskwaifu-bin"]).stdout.strip()}
    return DEFAULT_BIN_DIR
