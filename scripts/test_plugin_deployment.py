"""Regression tests for staging and packaging the independently loaded plugin."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("release_packaging", ROOT / "scripts/release-packaging.py")
PACKAGING = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGING)
TEMP_ROOT = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None


class PluginDeploymentTest(unittest.TestCase):
    # 测试目的：确认部署只消费现有产物，缺插件时不产生部分部署。
    # 测试方法：用两个 profile 的预建文件执行部署，在 PATH 放置禁止调用的 Cargo 替身。
    def test_stages_existing_plugins_in_both_profiles(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            (root / "scripts").mkdir()
            script = root / "scripts/deploy-plugins.sh"
            shutil.copy2(ROOT / "scripts/deploy-plugins.sh", script)
            tools = root / "tools"
            tools.mkdir()
            cargo = tools / "cargo"
            cargo.write_text("#!/bin/sh\nexit 99\n")
            cargo.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])
            env.pop("CARGO_TARGET_DIR", None)
            plugins = [f"libdeskwaifu_{name}.so" for name in ("test_plugin", "voice_input", "terminal")]
            for args, target, profile in (([], root / "target", "debug"),
                                          (["--release"], Path("../custom target"), "release")):
                with self.subTest(profile=profile):
                    if args:
                        env["CARGO_TARGET_DIR"] = str(target)
                    output = tools / target / profile
                    output.mkdir(parents=True)
                    for name in ["deskwaifu", *plugins[:-1]]:
                        (output / name).write_text(profile)
                    invocation = ["bash", str(script), *args]
                    result = subprocess.run(invocation, cwd=tools, env=env, capture_output=True)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(plugins[-1].encode(), result.stderr)
                    self.assertFalse((output / "plugins").exists())
                    (output / plugins[-1]).write_text(profile)
                    subprocess.run(invocation, cwd=tools, env=env, check=True)
                    for name in plugins:
                        self.assertEqual((output / "plugins" / name).read_text(), profile)
            result = subprocess.run(["bash", str(script), "--unknown"], env=env, capture_output=True)
            self.assertEqual(result.returncode, 2)

    # 测试目的：确认 Arch 安装布局可让宿主找到三个插件，且 CLI 独立可执行。
    # 测试方法：创建模拟构建产物并执行 PKGBUILD 的 package 函数，检查启动软链接、CLI 权限及插件内容。
    def test_package_installs_plugin_beside_real_executable(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            profile = root / "src/deskwaifu/target/release"
            profile.mkdir(parents=True)
            (profile / "deskwaifu").write_text("host")
            (profile / "deskwaifucli").write_text("cli")
            (profile / "deskwaifu-terminal-panel").write_text("panel")
            for name in ("test_plugin", "voice_input", "terminal"):
                (profile / f"libdeskwaifu_{name}.so").write_text("plugin")
            with tarfile.open(root / "deskwaifu.tar.gz", "w:gz") as archive:
                archive.add(ROOT / "Cargo.toml", arcname="deskwaifu/Cargo.toml")
            PACKAGING.prepare(root, "1.2.3", "a" * 40)
            subprocess.run(["bash", "-euc", 'source "$1"; package', "_",
                            str(root / "PKGBUILD")], check=True,
                           env=dict(os.environ, srcdir=str(root / "src"), pkgdir=str(root / "pkg")))
            launcher = root / "pkg/usr/bin/deskwaifu"
            self.assertTrue(launcher.is_symlink())
            self.assertEqual(launcher.resolve(), root / "pkg/usr/lib/deskwaifu/deskwaifu")
            cli = root / "pkg/usr/bin/deskwaifucli"
            self.assertFalse(cli.is_symlink())
            self.assertEqual(cli.read_text(), "cli")
            self.assertTrue(os.access(cli, os.X_OK))
            self.assertEqual((root / "pkg/usr/bin/deskwaifu-terminal-panel").read_text(), "panel")
            for name in ("test_plugin", "voice_input", "terminal"):
                self.assertEqual((launcher.resolve().parent / f"plugins/libdeskwaifu_{name}.so").read_text(),
                                 "plugin")


if __name__ == "__main__":
    unittest.main()
