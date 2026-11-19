"""Regression tests for staging and packaging the independently loaded plugin."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("release_packaging", ROOT / "scripts/release-packaging.py")
PACKAGING = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGING)
TEMP_ROOT = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None


class PluginDeploymentTest(unittest.TestCase):
    # 测试目的：确认调试和发布构建均部署两个动态插件，并支持自定义目标目录。
    # 测试方法：用 Cargo 替身生成带构建模式标记的插件文件，运行构建脚本并核对部署内容及无效参数的退出状态。
    def test_build_stages_both_profiles_and_custom_target(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            (root / "scripts").mkdir()
            shutil.copy2(ROOT / "scripts/build.sh", root / "scripts/build.sh")
            tools = root / "tools"
            tools.mkdir()
            cargo = tools / "cargo"
            cargo.write_text('''#!/usr/bin/env bash
set -euo pipefail
[[ "$1 $2 $3 $4" == 'build --workspace --locked --target-dir' ]]
target=$5
profile=debug
if [[ ${6:-} == --release ]]; then profile=release; fi
mkdir -p "$target/$profile"
for plugin in test_plugin voice_input; do
    printf 'plugin-%s' "$profile" > "$target/$profile/libdeskwaifu_$plugin.so"
done
''')
            cargo.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])
            env.pop("CARGO_TARGET_DIR", None)
            for args, target, profile in (([], root / "target", "debug"),
                                          (["--release"], root / "custom target", "release")):
                with self.subTest(profile=profile):
                    if args:
                        env["CARGO_TARGET_DIR"] = str(target)
                    subprocess.run(["bash", str(root / "scripts/build.sh"), *args],
                                   cwd=tools, env=env, check=True)
                    for name in ("test_plugin", "voice_input"):
                        plugin = target / profile / f"plugins/libdeskwaifu_{name}.so"
                        self.assertEqual(plugin.read_text(), "plugin-" + profile)
            result = subprocess.run(["bash", str(root / "scripts/build.sh"), "--unknown"],
                                    env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)

    # 测试目的：确认 Arch 安装布局可让宿主找到两个插件，且 CLI 独立可执行。
    # 测试方法：创建模拟构建产物并执行 PKGBUILD 的 package 函数，检查启动软链接、CLI 权限及插件内容。
    def test_package_installs_plugin_beside_real_executable(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            profile = root / "src/deskwaifu/target/release"
            (profile / "plugins").mkdir(parents=True)
            (profile / "deskwaifu").write_text("host")
            (profile / "deskwaifucli").write_text("cli")
            for name in ("test_plugin", "voice_input"):
                (profile / f"plugins/libdeskwaifu_{name}.so").write_text("plugin")
            subprocess.run(["bash", "-euc", 'source "$1"; package', "_",
                            str(ROOT / "packaging/release/PKGBUILD.in")], check=True,
                           env=dict(os.environ, srcdir=str(root / "src"), pkgdir=str(root / "pkg")))
            launcher = root / "pkg/usr/bin/deskwaifu"
            self.assertTrue(launcher.is_symlink())
            self.assertEqual(launcher.resolve(), root / "pkg/usr/lib/deskwaifu/deskwaifu")
            cli = root / "pkg/usr/bin/deskwaifucli"
            self.assertFalse(cli.is_symlink())
            self.assertEqual(cli.read_text(), "cli")
            self.assertTrue(os.access(cli, os.X_OK))
            for name in ("test_plugin", "voice_input"):
                self.assertEqual((launcher.resolve().parent / f"plugins/libdeskwaifu_{name}.so").read_text(),
                                 "plugin")

    # 测试目的：确认包内容校验要求宿主、启动入口、独立 CLI 和两个插件。
    # 测试方法：先校验完整文件清单，再逐项移除必需文件或加入旧插件，检查对应的拒绝原因。
    def test_payload_requires_host_launcher_cli_and_plugin(self):
        info = {"pkgname": ["deskwaifu-bin"], "pkgver": ["1.2.3-1"], "arch": ["x86_64"],
                "depend": sorted(PACKAGING.MAIN_DEPENDS)}
        files = {"usr/bin/deskwaifu", "usr/bin/deskwaifucli", "usr/lib/deskwaifu/deskwaifu",
                 "usr/lib/deskwaifu/plugins/libdeskwaifu_test_plugin.so",
                 "usr/lib/deskwaifu/plugins/libdeskwaifu_voice_input.so"}
        PACKAGING.verify_content("deskwaifu-bin", info, files, "1.2.3")
        for missing in files:
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, "Missing package files"):
                PACKAGING.verify_content("deskwaifu-bin", info, files - {missing}, "1.2.3")
        with self.assertRaisesRegex(ValueError, "Unexpected package files"):
            PACKAGING.verify_content("deskwaifu-bin", info,
                                     files | {"usr/lib/deskwaifu/plugins/legacy.so"}, "1.2.3")


if __name__ == "__main__":
    unittest.main()
