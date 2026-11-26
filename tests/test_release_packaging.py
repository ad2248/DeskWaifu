import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
TEMP_ROOT = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


packaging = load("release-packaging")
upload = load("release-upload")
COMMIT = "a" * 40
TAG = "v1.2.3"


def archive(path, files):
    with tarfile.open(path, "w:gz") as target:
        for name, content in files.items():
            data = content.encode()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            target.addfile(info, io.BytesIO(data))


def rust_fixture(*, voice=True, panel=False):
    """Offline Cargo workspace fixture with real binaries and cdylibs."""
    files = {
        "deskwaifu/src/main.rs": ('fn main() { assert_eq!(std::env::args().len(), 1); '
                                 'println!("host fixture"); }\n'),
        "deskwaifu/cli/src/main.rs": ('fn main() { assert_eq!(std::env::args().nth(1).as_deref(), '
                                     'Some("--help")); println!("cli fixture"); }\n'),
    }
    packages = {".": "deskwaifu", "cli": "deskwaifucli", "plugins/test-plugin": "deskwaifu-test-plugin"}
    if voice:
        packages["plugins/voice-input"] = "deskwaifu-voice-input"
    if panel:
        packages["plugins/terminal"] = "deskwaifu-terminal"
        packages["apps/terminal-panel"] = "deskwaifu-terminal-panel"
        files["deskwaifu/apps/terminal-panel/src/main.rs"] = 'fn main() { println!("panel fixture"); }\n'
    for directory, name in packages.items():
        path = "deskwaifu" if directory == "." else f"deskwaifu/{directory}"
        manifest = f'[package]\nname = "{name}"\nversion = "0.1.0"\nedition = "2021"\n'
        if directory.startswith("plugins/"):
            manifest += '[lib]\ncrate-type = ["cdylib"]\n'
            files[f"{path}/src/lib.rs"] = '#[no_mangle]\npub extern "C" fn fixture() {}\n'
        if directory == "plugins/terminal":
            manifest += '[features]\ndefault = ["kwin-cursor"]\nkwin-cursor = ["native"]\nnative = []\n'
            files[f"{path}/src/lib.rs"] += ('#[cfg(not(all(feature = "native", feature = "kwin-cursor")))]\n'
                                            'compile_error!("native terminal features required");\n')
        files[f"{path}/Cargo.toml"] = manifest
    files["deskwaifu/Cargo.toml"] = (f'[workspace]\nmembers = {json.dumps(list(packages))}\nresolver = "2"\n'
                                     + files["deskwaifu/Cargo.toml"])
    files["deskwaifu/Cargo.lock"] = "version = 4\n" + "".join(
        f'\n[[package]]\nname = "{name}"\nversion = "0.1.0"\n' for name in sorted(packages.values()))
    return files


class PackagingTest(unittest.TestCase):
    # 测试目的：验证真实 Arch 打包能交付宿主、CLI、面板和三个插件，并检查发行文件的校验和。
    # 测试方法：用 makepkg 构建离线 Rust 夹具，解包检查安装内容，再篡改包或添加旧包验证拒绝结果。
    @unittest.skipUnless(all(shutil.which(tool) for tool in ("makepkg", "bsdtar", "fakeroot", "cargo"))
                         and os.getuid() != 0, "requires Arch tools, Cargo and nonroot user")
    def test_real_makepkg_rust_package_and_checksums(self):
        with tempfile.TemporaryDirectory(prefix="release-rust-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            archive(work / "deskwaifu.tar.gz", rust_fixture(panel=True))
            payload = packaging.prepare(work, TAG, COMMIT)
            (work / "packages").mkdir()
            env = dict(os.environ, PKGDEST=str(work / "packages"), SOURCE_DATE_EPOCH="1700000000",
                       CARGO_HOME=str(work / "cargo-home"))
            result = subprocess.run(["makepkg", "--nodeps", "--noconfirm"], cwd=work, env=env,
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(payload, "rust-v3")
            packaging.verify(work / "packages", TAG, COMMIT, payload)
            self.assertEqual(len(upload.load_assets(work / "packages", TAG, COMMIT)), 3)
            package = next((work / "packages").glob("*.pkg.tar.zst"))
            installed = work / "installed"
            installed.mkdir()
            subprocess.run(["bsdtar", "-xf", str(package), "-C", str(installed), "usr/"], check=True)
            self.assert_installed_fixture(installed, panel=True)
            package.write_bytes(package.read_bytes() + b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                upload.load_assets(work / "packages", TAG, COMMIT)
            (work / "packages/deskwaifu-ocr-bin-old.pkg.tar.zst").write_bytes(b"stale")
            with self.assertRaisesRegex(ValueError, "Unexpected or missing"):
                packaging.verify(work / "packages", TAG, COMMIT, payload)

    def assert_installed_fixture(self, installed, *, voice=True, panel=False):
        executable = installed / "usr/bin/deskwaifu"
        self.assertTrue(executable.is_symlink())
        self.assertEqual(executable.resolve(), installed / "usr/lib/deskwaifu/deskwaifu")
        plugins = set((executable.resolve().parent / "plugins").iterdir())
        expected = {"libdeskwaifu_test_plugin.so"}
        if voice:
            expected.add("libdeskwaifu_voice_input.so")
        if panel:
            expected.add("libdeskwaifu_terminal.so")
        self.assertEqual({plugin.name for plugin in plugins}, expected)
        for plugin in plugins:
            self.assertTrue(plugin.read_bytes().startswith(b"\x7fELF"))
        self.assertEqual(subprocess.check_output([str(executable)], text=True), "host fixture\n")
        cli = installed / "usr/bin/deskwaifucli"
        self.assertFalse(cli.is_symlink())
        self.assertTrue(cli.read_bytes().startswith(b"\x7fELF"))
        self.assertEqual(subprocess.check_output([str(cli), "--help"], text=True), "cli fixture\n")
        panel_path = installed / "usr/bin/deskwaifu-terminal-panel"
        self.assertEqual(panel_path.exists(), panel)
        if panel:
            self.assertTrue(panel_path.read_bytes().startswith(b"\x7fELF"))
            self.assertEqual(subprocess.check_output([str(panel_path)], text=True), "panel fixture\n")

    # 测试目的：三个 Rust 载荷均直接离线构建，并要求 Cargo release 目录包含对应产物。
    # 测试方法：编译夹具，移走所需产物。即使 plugins 子目录有同名插件，打包也必须报错。
    @unittest.skipUnless(shutil.which("cargo"), "requires Cargo")
    def test_real_rust_fixture_build_and_install_without_arch_tools(self):
        variants = ((False, False, "rust-v1"), (True, False, "rust-v2"), (True, True, "rust-v3"))
        for voice, panel, expected_payload in variants:
            with self.subTest(payload=expected_payload), tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temporary:
                work = Path(temporary)
                fixture = rust_fixture(voice=voice, panel=panel)
                archive(work / "deskwaifu.tar.gz", fixture)
                payload = packaging.prepare(work, TAG, COMMIT)
                self.assertEqual(payload, expected_payload)
                for name, content in fixture.items():
                    path = work / "src" / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(content)
                installed = work / "installed"
                env = dict(os.environ, srcdir=str(work / "src"), pkgdir=str(installed),
                           CARGO_HOME=str(work / "cargo-home"), CARGO_TARGET_DIR=str(work / "wrong-target"))
                recipe = str(work / "PKGBUILD")
                result = subprocess.run(["bash", "-euc", 'source "$1"; build; package', "_", recipe],
                                        env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((work / "wrong-target").exists())
                self.assert_installed_fixture(installed, voice=voice, panel=panel)
                depends = subprocess.check_output(["bash", "-euc", 'source "$1"; printf "%s\n" "${depends[@]}"',
                                                   "_", recipe], text=True).splitlines()
                expected_depends = {"glibc", "gcc-libs", "libpipewire", "wl-clipboard"}
                if panel:
                    expected_depends |= {"libglvnd", "systemd-libs", "wayland", "fontconfig", "freetype2",
                                         "libxkbcommon", "libxkbcommon-x11", "libxcb", "libx11", "vulkan-icd-loader"}
                self.assertEqual(set(depends), expected_depends)
                self.assertEqual("cmake" in (work / "PKGBUILD").read_text(), panel)
                info = {"pkgname": ["deskwaifu-bin"], "pkgver": [TAG + "-1"], "arch": ["x86_64"],
                        "depend": depends}
                files = {str(path.relative_to(installed)) for path in installed.rglob("*") if not path.is_dir()}
                packaging.verify_content("deskwaifu-bin", info, files, TAG, payload)
                if panel:
                    for unwanted in ("usr/share/deskwaifu/Haru.model3.json", "usr/lib/libLive2DCubismCore.a",
                                     "usr/lib/deskwaifu/deskwaifu-cursor-guard"):
                        with self.assertRaisesRegex(ValueError, "Unexpected package files"):
                            packaging.verify_content("deskwaifu-bin", info, files | {unwanted}, TAG, payload)
                    old_dependencies = info | {"depend": sorted(packaging.MAIN_DEPENDS)}
                    with self.assertRaisesRegex(ValueError, "Unexpected Rust runtime dependencies"):
                        packaging.verify_content("deskwaifu-bin", old_dependencies, files, TAG, payload)

                if panel:
                    missing = ["usr/lib/deskwaifu/plugins/libdeskwaifu_terminal.so", "usr/bin/deskwaifu-terminal-panel"]
                elif voice:
                    missing = ["usr/lib/deskwaifu/plugins/libdeskwaifu_voice_input.so"]
                else:
                    missing = []
                output = work / "src/deskwaifu/target/release"
                for member in missing:
                    artifact = Path(member).name
                    if artifact.endswith(".so"):
                        staged = output / "plugins" / artifact
                        staged.parent.mkdir(exist_ok=True)
                        shutil.copyfile(output / artifact, staged)
                    (output / artifact).unlink()
                    # package() must propagate install errors even without `set -e`.
                    result = subprocess.run(["bash", "-uc", 'source "$1"; package', "_", recipe],
                                            env=dict(env, pkgdir=str(work / f"incomplete-{artifact}")),
                                            capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(artifact, result.stderr)
                    with self.assertRaisesRegex(ValueError, f"Missing package files: .*{artifact}"):
                        packaging.verify_content("deskwaifu-bin", info, files - {member}, TAG, payload)

    def test_rust_release_uses_offline_locked_workspace_build(self):
        recipe = (ROOT / "packaging/release/PKGBUILD.in").read_text()
        self.assertIn("CARGO_NET_OFFLINE=true cargo build --workspace --locked --release", recipe)
        self.assertNotIn("scripts/build.sh", recipe)
        self.assertIn('"$pkgdir/usr/bin/deskwaifu"', recipe)
        self.assertNotIn("private-deps", recipe)
        workflow = (ROOT / ".gitea/workflows/release.yml").read_text()
        self.assertIn("runs-on: deskwaifu-rust", workflow)
        self.assertNotIn("prune-tests", workflow)
        self.assertNotIn("pacman -", workflow)
        script = (ROOT / "scripts/release-build.sh").read_text()
        self.assertIn("env -i PATH=/usr/bin:/bin", script)
        self.assertNotRegex(script, r"(?m)^\s*sudo\s")

    # 测试目的：原生构建输入只进入 v3 的离线编译，未获授权的公开构建在拉取依赖前停止。
    # 测试方法：以假 cargo/makepkg 捕获真实 env -i 的变量，模拟不同载荷、缺失目录和拒绝授权。
    def test_release_fetch_is_unprivileged_and_shares_offline_build_cache(self):
        script = (ROOT / "scripts/release-build.sh").read_text()
        phase = script[script.index('# Public release builds'):script.index('output="$root/release-assets"')]
        with tempfile.TemporaryDirectory(prefix="release-fetch-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            tools = work / "tools"
            tools.mkdir()
            (work / "home").mkdir()
            archive(work / "deskwaifu.tar.gz", rust_fixture())
            log = work / "calls.jsonl"
            # Emulate only the privilege transition: execute the actual env -i
            # and shell phase, substituting observable cargo/makepkg commands.
            runuser = tools / "runuser"
            runuser.write_text(f'''#!{sys.executable}
import subprocess, sys
assert sys.argv[1:4] == ["-u", "fixture-user", "--"]
command = sys.argv[4:]
assert command[:3] == ["env", "-i", "PATH=/usr/bin:/bin"]
command[2] = "PATH={tools}:/usr/bin:/bin"
sys.exit(subprocess.run(command).returncode)
''')
            runuser.chmod(0o755)
            for name in ("cargo", "makepkg"):
                tool = tools / name
                tool.write_text(f'''#!{sys.executable}
import json, os, pathlib, sys
home = pathlib.Path(os.environ["CARGO_HOME"])
home.mkdir(parents=True, exist_ok=True)
if {name!r} == "cargo":
    assert sys.argv[1:] == ["fetch", "--locked", "--manifest-path", "fetch-source/deskwaifu/Cargo.toml"]
    assert pathlib.Path(sys.argv[-1]).is_file()
    assert "CARGO_NET_OFFLINE" not in os.environ
    assert os.environ["https_proxy"] == "http://proxy.invalid:1234"
    (home / "fetched").touch()
else:
    assert (home / "fetched").exists()
    assert os.environ["CARGO_NET_OFFLINE"] == "true"
    assert "https_proxy" not in os.environ
    assert "RUSTC_WRAPPER" not in os.environ
    assert "SCCACHE_DIR" not in os.environ
assert "RELEASE_UPLOAD" not in os.environ
assert "GITEA_TOKEN" not in os.environ
assert "CUBISM_LAYER_SHELL_XML" not in os.environ
assert "DESKWAIFU_CUBISM_REDISTRIBUTION_APPROVED" not in os.environ
sdk = {{key: value for key, value in os.environ.items() if key.startswith("CUBISM_")}}
with open({str(log)!r}, "a") as stream:
    stream.write(json.dumps({{"tool": {name!r}, "home": str(home), "sdk": sdk}}) + "\\n")
''')
                tool.chmod(0o755)
            sdk = {"CUBISM_SDK_DIR": str(work / "Cubism SDK"), "CUBISM_GLEW_DIR": str(work / "GLEW sources")}
            for value in sdk.values():
                Path(value).mkdir()
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       build_user="fixture-user", work=str(work), SOURCE_DATE_EPOCH="1700000000",
                       RELEASE_UPLOAD="must-not-leak", GITEA_TOKEN="must-not-leak",
                       CUBISM_LAYER_SHELL_XML="must-not-leak",
                       RUSTC_WRAPPER="sccache", SCCACHE_DIR="/cache/sccache-rust-package",
                       DESKWAIFU_CI_OFFLINE="0", DESKWAIFU_CI_SCCACHE="1", RELEASE_TOOLS_DIR=str(work),
                       https_proxy="http://proxy.invalid:1234", **sdk)

            def run(**overrides):
                return subprocess.run(["bash", "-euc", phase], env=env | overrides, capture_output=True, text=True)

            for payload, public, approval in (("rust-v1", "1", ""), ("rust-v2", "1", ""),
                                              ("rust-v3", "0", ""), ("rust-v3", "1", "1")):
                with self.subTest(payload=payload, public=public):
                    result = run(payload=payload, DESKWAIFU_PUBLIC_RELEASE=public,
                                 DESKWAIFU_CUBISM_REDISTRIBUTION_APPROVED=approval)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    calls = [json.loads(line) for line in log.read_text().splitlines()]
                    self.assertEqual([call["tool"] for call in calls], ["cargo", "makepkg"])
                    self.assertEqual({call["home"] for call in calls}, {str(work / "home/.cargo")})
                    self.assertEqual(calls[0]["sdk"], {})
                    self.assertEqual(calls[1]["sdk"], sdk if payload == "rust-v3" else {})
                    self.assertFalse((work / "fetch-source").exists())
                    log.unlink()
                    (work / "home/.cargo/fetched").unlink()

            result = run(payload="rust-v3", DESKWAIFU_PUBLIC_RELEASE="1", DESKWAIFU_CUBISM_REDISTRIBUTION_APPROVED="")
            self.assertEqual(result.returncode, 1)
            self.assertIn("redistribution authorization", result.stderr)
            self.assertFalse(log.exists())
            result = run(payload="rust-v3", DESKWAIFU_PUBLIC_RELEASE="0", CUBISM_GLEW_DIR="")
            self.assertEqual(result.returncode, 1)
            self.assertIn("Set CUBISM_GLEW_DIR", result.stderr)
            self.assertFalse(log.exists())
            result = run(payload="rust-v3", DESKWAIFU_PUBLIC_RELEASE="0", CUBISM_SDK_DIR="relative/path")
            self.assertEqual(result.returncode, 1)
            self.assertIn("Set CUBISM_SDK_DIR", result.stderr)
            self.assertFalse(log.exists())
            # A failed dependency fetch must never proceed to compilation.
            (tools / "cargo").write_text('#!/bin/sh\nexit 23\n')
            result = run(payload="rust-v2")
            self.assertEqual(result.returncode, 23)
            self.assertFalse(log.exists())

    # 测试目的：打包工具取自工作流，载荷取自所选标签的源码清单。
    # 测试方法：标签夹具只含测试插件，分支夹具包含面板。检出标签后，用工作流工具生成配方并核对内容。
    def test_workflow_tools_package_the_selected_tag_payload(self):
        with tempfile.TemporaryDirectory(prefix="release-historical-tools-", dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            repo = root / "source"
            repo.mkdir()
            def git(*args):
                return subprocess.check_output(["git", "-C", str(repo), *args], text=True, stderr=subprocess.PIPE).strip()
            git("init", "--initial-branch=main")
            (repo / "scripts").mkdir()
            historical = repo / "scripts/release-packaging.py"
            historical.write_text('raise RuntimeError("historical packaging must not run")\n')
            (repo / "Cargo.toml").write_text(rust_fixture(voice=False)["deskwaifu/Cargo.toml"])
            git("add", ".")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "historical")
            commit = git("rev-parse", "HEAD")
            git("tag", "0.1.0")
            historical.write_text("# newer source\n")
            (repo / "Cargo.toml").write_text(rust_fixture(panel=True)["deskwaifu/Cargo.toml"])
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-am", "later")
            env_file = root / "env"
            env = dict(os.environ, RELEASE_EVENT="workflow_dispatch", RELEASE_REF="refs/heads/main",
                       RELEASE_INPUT_TAG="0.1.0", RELEASE_NOTES="## 更新\n- 历史版本发布\n",
                       GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/release-source.py")],
                                    cwd=repo, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            values = dict(line.split("=", 1) for line in env_file.read_text().splitlines())
            tools = Path(values["RELEASE_TOOLS_DIR"])
            self.assertFalse(tools.is_relative_to(repo))
            self.assertIn("historical packaging must not run", historical.read_text())
            acquired = root / "acquired"
            acquired.mkdir()
            (acquired / "deskwaifu.tar.gz").write_bytes(subprocess.check_output(
                ["git", "archive", "--format=tar.gz", "--prefix=deskwaifu/", commit], cwd=repo))
            payload = subprocess.check_output([sys.executable, "-B", str(tools / "scripts/release-packaging.py"),
                                               "prepare", str(acquired), "0.1.0", commit], text=True).strip()
            self.assertEqual(payload, "rust-v1")
            recipe = (acquired / "PKGBUILD").read_text()
            self.assertIn("pkgver=0.1.0\n", recipe)
            self.assertNotIn("libdeskwaifu_voice_input.so", recipe)
            self.assertNotIn("deskwaifu-terminal-panel", recipe)
            self.assertNotIn("libglvnd", recipe)
            self.assertEqual(git("rev-parse", "HEAD"), commit)

    def test_arbitrary_tags_are_used_as_pkgver_without_conversion(self):
        for tag in ("0.1.0", "v0.1.0", "nightly", "release/2026-10", "0.1.0-rc.1", "发布",
                    "0.1.0+build", "@SOURCE_SHA@", "@PLUGINS@", "tag'$(touch SHOULD_NOT_EXIST)"):
            with self.subTest(tag=tag):
                with tempfile.TemporaryDirectory(prefix="release-raw-tag-", dir=TEMP_ROOT) as temporary:
                    work = Path(temporary)
                    archive(work / "deskwaifu.tar.gz", rust_fixture())
                    packaging.prepare(work, tag, COMMIT)
                    result = subprocess.check_output(["bash", "-c", '. "$1"; printf "%s" "$pkgver"',
                                                      "_", str(work / "PKGBUILD")], cwd=work, text=True)
                    self.assertEqual(result, tag)
                    self.assertFalse((work / "SHOULD_NOT_EXIST").exists())

    @unittest.skipUnless(shutil.which("makepkg") and os.getuid() != 0, "requires nonroot makepkg")
    def test_arch_incompatible_tags_fail_in_makepkg_without_fallback(self):
        with tempfile.TemporaryDirectory(prefix="release-invalid-pkgver-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            archive(work / "deskwaifu.tar.gz", rust_fixture())
            for tag in ("release/2026", "v1.2.3-rc.1", "发布"):
                with self.subTest(tag=tag):
                    packaging.prepare(work, tag, COMMIT)
                    result = subprocess.run(["makepkg", "--printsrcinfo"], cwd=work, capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("pkgver", result.stderr)

    def test_actual_checkout_is_detached_tag_not_dispatch_branch(self):
        with tempfile.TemporaryDirectory(prefix="release-source-", dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            origin = root / "origin"
            origin.mkdir()

            def git(*args):
                return subprocess.check_output(["git", "-C", str(origin), *args], text=True, stderr=subprocess.PIPE).strip()

            git("init", "--initial-branch=main")
            (origin / "version").write_text("tagged source")
            git("add", "version")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "release")
            commit = git("rev-parse", "HEAD")
            git("tag", TAG)
            git("tag", "1.2.3")
            git("tag", "release/2026-10")
            (origin / "version").write_text("newer branch source, must not be packaged")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-am", "later")
            checkout = root / "checkout"
            subprocess.run(["git", "clone", "--quiet", str(origin), str(checkout)], check=True)
            env_file = root / "env"
            env = dict(os.environ, RELEASE_EVENT="workflow_dispatch", RELEASE_REF="refs/heads/main",
                       RELEASE_INPUT_TAG=TAG, RELEASE_NOTES="发布说明",
                       GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
            command = [sys.executable, "-B", str(ROOT / "scripts/release-source.py")]
            result = subprocess.run(command, cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((checkout / "version").read_text(), "tagged source")
            self.assertIn(f"RELEASE_COMMIT={commit}", env_file.read_text())
            symbolic = subprocess.run(["git", "symbolic-ref", "-q", "HEAD"], cwd=checkout, capture_output=True)
            self.assertNotEqual(symbolic.returncode, 0)
            env.update(RELEASE_INPUT_TAG="1.2.3")
            result = subprocess.run(command, cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("RELEASE_TAG=1.2.3\n", env_file.read_text())
            self.assertEqual((checkout / "version").read_text(), "tagged source")
            # A moved tag/push identity must fail before changing source.
            env.update(RELEASE_INPUT_TAG="release/2026-10")
            result = subprocess.run(command, cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("RELEASE_TAG=release/2026-10\n", env_file.read_text())
            env.update(RELEASE_EVENT="push", RELEASE_REF="refs/tags/" + TAG, RELEASE_EVENT_SHA=git("rev-parse", "HEAD"))
            result = subprocess.run(command, cwd=checkout, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("moved since the push event", result.stderr)
            env.update(RELEASE_EVENT_SHA=commit, RELEASE_NOTES="")
            result = subprocess.run(command, cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("RELEASE_NOTES=", env_file.read_text())

    def test_dispatch_creates_missing_tag_on_latest_main(self):
        with tempfile.TemporaryDirectory(prefix="release-create-tag-", dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            origin = root / "origin"
            origin.mkdir()

            def git(*args):
                return subprocess.check_output(["git", "-C", str(origin), *args], text=True,
                                               stderr=subprocess.PIPE).strip()

            git("init", "--initial-branch=main")
            (origin / "version").write_text("older source")
            git("add", "version")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "first")
            git("tag", "0.1.0")
            (origin / "version").write_text("latest main source")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-am", "second")
            main_head = git("rev-parse", "HEAD")
            checkout = root / "checkout"
            subprocess.run(["git", "clone", "--quiet", str(origin), str(checkout)], check=True)
            env_file = root / "env"
            env = dict(os.environ, RELEASE_EVENT="workflow_dispatch", RELEASE_REF="refs/heads/main",
                       RELEASE_INPUT_TAG="0.2.0", RELEASE_NOTES="发布说明",
                       RELEASE_NOTES_ONLY="false", GITEA_TOKEN="", GITHUB_TOKEN="",
                       GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/release-source.py")],
                                    cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(git("rev-parse", "0.2.0"), main_head)
            self.assertEqual((checkout / "version").read_text(), "latest main source")
            self.assertIn(f"RELEASE_TAG=0.2.0\nRELEASE_COMMIT={main_head}", env_file.read_text())
            symbolic = subprocess.run(["git", "symbolic-ref", "-q", "HEAD"], cwd=checkout, capture_output=True)
            self.assertNotEqual(symbolic.returncode, 0)

    def test_dispatch_existing_tag_is_never_moved_by_main_progress(self):
        with tempfile.TemporaryDirectory(prefix="release-existing-tag-", dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            origin = root / "origin"
            origin.mkdir()

            def git(*args):
                return subprocess.check_output(["git", "-C", str(origin), *args], text=True,
                                               stderr=subprocess.PIPE).strip()

            git("init", "--initial-branch=main")
            (origin / "version").write_text("tagged source")
            git("add", "version")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "release")
            tagged = git("rev-parse", "HEAD")
            git("tag", TAG)
            (origin / "version").write_text("newer main source, must not be released")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-am", "later")
            checkout = root / "checkout"
            subprocess.run(["git", "clone", "--quiet", str(origin), str(checkout)], check=True)
            env_file = root / "env"
            env = dict(os.environ, RELEASE_EVENT="workflow_dispatch", RELEASE_REF="refs/heads/main",
                       RELEASE_INPUT_TAG=TAG, RELEASE_NOTES="发布说明",
                       GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/release-source.py")],
                                    cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(git("rev-parse", TAG), tagged)
            self.assertEqual((checkout / "version").read_text(), "tagged source")

    def test_push_event_and_notes_only_never_create_missing_tag(self):
        with tempfile.TemporaryDirectory(prefix="release-missing-tag-", dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            origin = root / "origin"
            origin.mkdir()

            def git(*args):
                return subprocess.check_output(["git", "-C", str(origin), *args], text=True,
                                               stderr=subprocess.PIPE).strip()

            git("init", "--initial-branch=main")
            (origin / "version").write_text("source")
            git("add", "version")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "only")
            head = git("rev-parse", "HEAD")
            checkout = root / "checkout"
            subprocess.run(["git", "clone", "--quiet", str(origin), str(checkout)], check=True)
            env_file = root / "env"
            env = dict(os.environ, RELEASE_EVENT="push", RELEASE_REF="refs/tags/0.9.0",
                       RELEASE_EVENT_SHA=head, RELEASE_NOTES="",
                       GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
            command = [sys.executable, "-B", str(ROOT / "scripts/release-source.py")]
            result = subprocess.run(command, cwd=checkout, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("does not exist", result.stderr)
            env.update(RELEASE_EVENT="workflow_dispatch", RELEASE_REF="refs/heads/main",
                       RELEASE_INPUT_TAG="0.9.0", RELEASE_NOTES="发布说明", RELEASE_NOTES_ONLY="true")
            result = subprocess.run(command, cwd=checkout, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("existing tag", result.stderr)
            self.assertEqual(git("tag", "--list", "0.9.0"), "")

    def test_dispatch_tag_creation_survives_special_characters(self):
        tag = "0.2.0-rc.1"
        with tempfile.TemporaryDirectory(prefix="release-tag-chars-", dir=TEMP_ROOT) as temporary:
            root = Path(temporary)
            origin = root / "origin"
            origin.mkdir()

            def git(*args):
                return subprocess.check_output(["git", "-C", str(origin), *args], text=True,
                                               stderr=subprocess.PIPE).strip()

            git("init", "--initial-branch=main")
            (origin / "version").write_text("source")
            git("add", "version")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "only")
            head = git("rev-parse", "HEAD")
            checkout = root / "checkout"
            subprocess.run(["git", "clone", "--quiet", str(origin), str(checkout)], check=True)
            env = dict(os.environ, RELEASE_EVENT="workflow_dispatch", RELEASE_REF="refs/heads/main",
                       RELEASE_INPUT_TAG=tag, RELEASE_NOTES="发布说明",
                       GITHUB_ENV=str(root / "env"), RUNNER_TEMP=str(root))
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/release-source.py")],
                                    cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(git("rev-parse", tag), head)

    def test_workflow_secret_is_scoped_to_final_step(self):
        workflow = (ROOT / ".gitea/workflows/release.yml").read_text()
        self.assertEqual(workflow.count("secrets.RELEASE_UPLOAD"), 1)
        self.assertNotIn("secrets.RELEASE_UPLOAD", workflow.split("- name: Upload verified packages")[0])
        self.assertIn("runs-on: deskwaifu-rust", workflow)
        self.assertIn("tags: ['[0-9]*.[0-9]*.[0-9]*']", workflow)
        self.assertIn("https://git.kals.top/actions/checkout@v4", workflow)
        self.assertIn("RELEASE_INPUT_TAG: ${{ github.event.inputs.tag }}", workflow)
        self.assertIn("      release_notes:\n", workflow)
        notes_input = workflow.split("      release_notes:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("required: true", notes_input)
        self.assertIn("type: string", notes_input)
        self.assertEqual(workflow.count("RELEASE_NOTES: ${{ github.event.inputs.release_notes }}"), 2)
        self.assertNotIn("${{ github.event.inputs.release_notes }}", "\n".join(
            line for line in workflow.splitlines() if "RELEASE_NOTES:" not in line))
        self.assertNotIn('python3 -B "$RELEASE_TOOLS_DIR/tests/test_release_packaging.py"', workflow)
        release_build_step = workflow.split("- name: Acquire verified sources", 1)[1].split("- name: Upload", 1)[0]
        self.assertIn('DESKWAIFU_PUBLIC_RELEASE: \'1\'', release_build_step)
        self.assertIn("DESKWAIFU_CUBISM_REDISTRIBUTION_APPROVED: ${{ vars.DESKWAIFU_CUBISM_REDISTRIBUTION_APPROVED }}",
                      release_build_step)
        self.assertIn('bash "$RELEASE_TOOLS_DIR/scripts/release-build.sh"', release_build_step)
        self.assertIn('python3 "$RELEASE_TOOLS_DIR/scripts/release-upload.py"', workflow)
        job_env = workflow.split("    env:\n", 1)[1].split("    steps:", 1)[0]
        for name in ("http_proxy", "https_proxy", "ALL_PROXY"):
            self.assertIn(f"{name}: http://100.64.0.11:7912", job_env)
        self.assertIn("no_proxy: 127.0.0.1,localhost", job_env)
        subprocess.run(["bash", "-n", str(ROOT / "scripts/release-build.sh")], check=True)

    def test_nonroot_release_orchestration_fails_before_acquisition(self):
        with tempfile.TemporaryDirectory(prefix="release-nonroot-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            fake_id = work / "id"
            fake_id.write_text("#!/bin/bash\necho 1000\n")
            fake_id.chmod(0o755)
            env = dict(os.environ, PATH=f"{work}:{os.environ['PATH']}")
            env.pop("RELEASE_TAG", None)
            env.pop("RELEASE_COMMIT", None)
            result = subprocess.run(["bash", str(ROOT / "scripts/release-build.sh")], cwd=work, env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("打包需要 root 编排", result.stderr)
            self.assertNotIn("RELEASE_TAG", result.stderr)


if __name__ == "__main__":
    unittest.main()
