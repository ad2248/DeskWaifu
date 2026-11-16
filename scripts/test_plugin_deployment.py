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
printf 'plugin-%s' "$profile" > "$target/$profile/libdeskwaifu_test_plugin.so"
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
                    plugin = target / profile / "plugins/libdeskwaifu_test_plugin.so"
                    self.assertEqual(plugin.read_text(), "plugin-" + profile)
            result = subprocess.run(["bash", str(root / "scripts/build.sh"), "--unknown"],
                                    env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)

    def test_package_installs_plugin_beside_real_executable(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            profile = root / "src/deskwaifu/target/release"
            (profile / "plugins").mkdir(parents=True)
            (profile / "deskwaifu").write_text("host")
            (profile / "deskwaifucli").write_text("cli")
            (profile / "plugins/libdeskwaifu_test_plugin.so").write_text("plugin")
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
            self.assertEqual((launcher.resolve().parent / "plugins/libdeskwaifu_test_plugin.so").read_text(),
                             "plugin")

    def test_payload_requires_host_launcher_cli_and_plugin(self):
        info = {"pkgname": ["deskwaifu-bin"], "pkgver": ["1.2.3-1"], "arch": ["x86_64"],
                "depend": sorted(PACKAGING.MAIN_DEPENDS)}
        files = {"usr/bin/deskwaifu", "usr/bin/deskwaifucli", "usr/lib/deskwaifu/deskwaifu",
                 "usr/lib/deskwaifu/plugins/libdeskwaifu_test_plugin.so"}
        PACKAGING.verify_content("deskwaifu-bin", info, files, "1.2.3")
        for missing in files:
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, "Missing package files"):
                PACKAGING.verify_content("deskwaifu-bin", info, files - {missing}, "1.2.3")
        with self.assertRaisesRegex(ValueError, "Unexpected package files"):
            PACKAGING.verify_content("deskwaifu-bin", info,
                                     files | {"usr/lib/deskwaifu/plugins/legacy.so"}, "1.2.3")


if __name__ == "__main__":
    unittest.main()
