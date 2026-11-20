import hashlib
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
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.response import addinfourl

ROOT = Path(__file__).resolve().parent.parent
TEMP_ROOT = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


source = load("release-source")
packaging = load("release-packaging")
upload = load("release-upload")
COMMIT = "a" * 40
OTHER = "b" * 40
TAG = "v1.2.3"


def archive(path, files):
    with tarfile.open(path, "w:gz") as target:
        for name, content in files.items():
            data = content.encode()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            target.addfile(info, io.BytesIO(data))


def rust_fixture(*, voice=True):
    """Offline workspace fixture exercising the real host + CLI + cdylib recipe."""
    files = {
        "deskwaifu/Cargo.toml": ('[workspace]\nmembers = [".", "cli", "plugins/test-plugin", "plugins/voice-input"]\nresolver = "2"\n'
                                '[package]\nname="deskwaifu"\nversion="0.1.0"\nedition="2021"\n'),
        "deskwaifu/Cargo.lock": ('version = 4\n\n[[package]]\nname = "deskwaifu"\nversion = "0.1.0"\n'
                                '\n[[package]]\nname = "deskwaifu-test-plugin"\nversion = "0.1.0"\n'
                                '\n[[package]]\nname = "deskwaifu-voice-input"\nversion = "0.1.0"\n'
                                '\n[[package]]\nname = "deskwaifucli"\nversion = "0.1.0"\n'),
        "deskwaifu/src/main.rs": ('fn main() { assert_eq!(std::env::args().len(), 1); '
                                 'println!("host fixture"); }\n'),
        "deskwaifu/cli/Cargo.toml": ('[package]\nname="deskwaifucli"\nversion="0.1.0"\n'
                                    'edition="2021"\n'),
        "deskwaifu/cli/src/main.rs": ('fn main() { assert_eq!(std::env::args().nth(1).as_deref(), '
                                     'Some("--help")); println!("cli fixture"); }\n'),
        "deskwaifu/plugins/test-plugin/Cargo.toml": ('[package]\nname="deskwaifu-test-plugin"\nversion="0.1.0"\n'
                                       'edition="2021"\n[lib]\ncrate-type=["rlib", "cdylib"]\n'),
        "deskwaifu/plugins/test-plugin/src/lib.rs": '#[no_mangle]\npub extern "C" fn fixture() {}\n',
        "deskwaifu/plugins/voice-input/Cargo.toml": ('[package]\nname="deskwaifu-voice-input"\nversion="0.1.0"\n'
                                       'edition="2021"\n[lib]\ncrate-type=["rlib", "cdylib"]\n'),
        "deskwaifu/plugins/voice-input/src/lib.rs": '#[no_mangle]\npub extern "C" fn fixture() {}\n',
        "deskwaifu/scripts/build.sh": (ROOT / "scripts/build.sh").read_text(),
    }
    if not voice:
        del files["deskwaifu/plugins/voice-input/Cargo.toml"], files["deskwaifu/plugins/voice-input/src/lib.rs"]
        files["deskwaifu/Cargo.toml"] = files["deskwaifu/Cargo.toml"].replace(', "plugins/voice-input"', "")
        files["deskwaifu/Cargo.lock"] = files["deskwaifu/Cargo.lock"].replace(
            '\n[[package]]\nname = "deskwaifu-voice-input"\nversion = "0.1.0"\n', "")
        files["deskwaifu/scripts/build.sh"] = files["deskwaifu/scripts/build.sh"].replace(
            " libdeskwaifu_voice_input.so", "")
    return files


class PackagingTest(unittest.TestCase):
    def test_control_characters_cannot_inject_release_environment(self):
        for tag in ("v1\nRELEASE_COMMIT=evil", "v1\rbroken", "v1\0broken"):
            for event, ref, explicit in (("workflow_dispatch", "", tag), ("push", "refs/tags/" + tag, "")):
                with self.subTest(tag=tag, event=event), self.assertRaisesRegex(ValueError, "control characters"):
                    source.select_tag(event, ref, explicit)

    def test_internal_dependencies_are_unversioned(self):
        packaging.verify_unversioned_dependencies(["deskwaifu-bin"])
        for operator in ("=", ">=", "<=", ">", "<"):
            with self.assertRaisesRegex(ValueError, "without a version constraint"):
                packaging.verify_unversioned_dependencies(["deskwaifu-bin" + operator + "1.0"])

    # 测试目的：确认发行包身份、依赖和文件清单符合约定，并要求语音识别插件随包交付。
    # 测试方法：校验合法包，再替换元数据、加入额外文件或逐项移除必需文件，检查拒绝结果。
    def test_single_rust_payload_and_identity(self):
        info = {"pkgname": ["deskwaifu-bin"], "pkgver": ["1.2.3-1"], "arch": ["x86_64"],
                "depend": ["glibc", "gcc-libs", "libpipewire", "wl-clipboard"]}
        required = {"usr/bin/deskwaifu", "usr/bin/deskwaifucli", "usr/lib/deskwaifu/deskwaifu",
                    "usr/lib/deskwaifu/plugins/libdeskwaifu_test_plugin.so",
                    "usr/lib/deskwaifu/plugins/libdeskwaifu_voice_input.so"}
        files = required | {"usr/", "usr/bin/", "usr/lib/", "usr/lib/deskwaifu/",
                            "usr/lib/deskwaifu/plugins/", ".PKGINFO", ".BUILDINFO", ".MTREE"}
        packaging.verify_content("deskwaifu-bin", info, files, "1.2.3", "rust-v2")
        for field, value in (("pkgname", ["deskwaifu-ocr-bin"]), ("pkgver", ["2.0-1"]),
                             ("arch", ["any"]), ("depend", ["qt6-base"])):
            with self.subTest(field=field), self.assertRaises(ValueError):
                packaging.verify_content("deskwaifu-bin", dict(info, **{field: value}), files, "1.2.3", "rust-v2")
        for extra in ("usr/lib/deskwaifu/plugins/model.so", "usr/share/models/model.onnx",
                      "usr/lib/deskwaifu/tests/run.py", "../escape", "usr/bin/deskwaifu-control"):
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, "Unexpected package files"):
                packaging.verify_content("deskwaifu-bin", info, files | {extra}, "1.2.3", "rust-v2")
        for missing in required:
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, "Missing package files"):
                packaging.verify_content("deskwaifu-bin", info, files - {missing}, "1.2.3", "rust-v2")

    # 测试目的：验证真实 Arch 打包能交付宿主、CLI 和两个插件，并检查发行文件的校验和。
    # 测试方法：用 makepkg 构建离线 Rust 夹具，解包检查安装内容，再篡改包或添加旧包验证拒绝结果。
    @unittest.skipUnless(all(shutil.which(tool) for tool in ("makepkg", "bsdtar", "fakeroot", "cargo"))
                         and os.getuid() != 0, "requires Arch tools, Cargo and nonroot user")
    def test_real_makepkg_rust_package_and_checksums(self):
        with tempfile.TemporaryDirectory(prefix="release-rust-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            archive(work / "deskwaifu.tar.gz", rust_fixture())
            payload = packaging.prepare(work, TAG, COMMIT)
            env = dict(os.environ, PKGDEST=str(work / "packages"), SOURCE_DATE_EPOCH="1700000000",
                       CARGO_HOME=str(work / "cargo-home"), CARGO_TARGET_DIR="target")
            result = subprocess.run(["makepkg", "--nodeps", "--noconfirm"], cwd=work, env=env,
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            packaging.verify(work / "packages", TAG, COMMIT, payload)
            self.assertEqual(len(upload.load_assets(work / "packages", TAG, COMMIT)), 3)
            package = next((work / "packages").glob("*.pkg.tar.zst"))
            installed = work / "installed"
            installed.mkdir()
            subprocess.run(["bsdtar", "-xf", str(package), "-C", str(installed), "usr/"], check=True)
            self.assert_installed_fixture(installed)
            package.write_bytes(package.read_bytes() + b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                upload.load_assets(work / "packages", TAG, COMMIT)
            (work / "packages/deskwaifu-ocr-bin-old.pkg.tar.zst").write_bytes(b"stale")
            with self.assertRaisesRegex(ValueError, "Unexpected or missing"):
                packaging.verify(work / "packages", TAG, COMMIT, payload)

    def assert_installed_fixture(self, installed, *, voice=True):
        executable = installed / "usr/bin/deskwaifu"
        self.assertTrue(executable.is_symlink())
        self.assertEqual(executable.resolve(), installed / "usr/lib/deskwaifu/deskwaifu")
        plugins = set((executable.resolve().parent / "plugins").iterdir())
        expected = {"libdeskwaifu_test_plugin.so"}
        if voice:
            expected.add("libdeskwaifu_voice_input.so")
        self.assertEqual({plugin.name for plugin in plugins}, expected)
        for plugin in plugins:
            self.assertTrue(plugin.read_bytes().startswith(b"\x7fELF"))
        self.assertEqual(subprocess.check_output([str(executable)], text=True), "host fixture\n")
        cli = installed / "usr/bin/deskwaifucli"
        self.assertFalse(cli.is_symlink())
        self.assertTrue(cli.read_bytes().startswith(b"\x7fELF"))
        self.assertEqual(subprocess.check_output([str(cli), "--help"], text=True), "cli fixture\n")

    # 测试目的：确认当前配方可打包旧版和语音版 Rust 源码，且语音版缺少插件时必须失败。
    # 测试方法：用两版源码存档生成配方并离线构建，核对安装和校验清单；删除语音版产物后重试打包与校验。
    @unittest.skipUnless(shutil.which("cargo"), "requires Cargo")
    def test_real_rust_fixture_build_and_install_without_arch_tools(self):
        for voice, expected_payload in ((False, "rust-v1"), (True, "rust-v2")):
            with self.subTest(voice=voice), tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temporary:
                work = Path(temporary)
                fixture = rust_fixture(voice=voice)
                archive(work / "deskwaifu.tar.gz", fixture)
                payload = packaging.prepare(work, TAG, COMMIT)
                for name, content in fixture.items():
                    path = work / "src" / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(content)
                installed = work / "installed"
                env = dict(os.environ, srcdir=str(work / "src"), pkgdir=str(installed),
                           CARGO_HOME=str(work / "cargo-home"), CARGO_TARGET_DIR="target")
                recipe = str(work / "PKGBUILD")
                result = subprocess.run(["bash", "-euc", 'source "$1"; build; package', "_", recipe],
                                        env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(payload, expected_payload)
                self.assert_installed_fixture(installed, voice=voice)
                info = {"pkgname": ["deskwaifu-bin"], "pkgver": [TAG + "-1"], "arch": ["x86_64"],
                        "depend": sorted(packaging.MAIN_DEPENDS)}
                files = {str(path.relative_to(installed)) for path in installed.rglob("*") if not path.is_dir()}
                packaging.verify_content("deskwaifu-bin", info, files, TAG, payload)

                if voice:
                    plugin = "libdeskwaifu_voice_input.so"
                    (work / "src/deskwaifu/target/release/plugins" / plugin).unlink()
                    # package() must propagate install errors even without `set -e`.
                    result = subprocess.run(["bash", "-uc", 'source "$1"; package', "_", recipe],
                                            env=dict(env, pkgdir=str(work / "incomplete")),
                                            capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(plugin, result.stderr)
                    with self.assertRaisesRegex(ValueError, f"Missing package files: .*{plugin}"):
                        packaging.verify_content("deskwaifu-bin", info,
                                                 files - {f"usr/lib/deskwaifu/plugins/{plugin}"}, TAG, payload)

    def test_rust_release_uses_offline_locked_build_without_private_dependencies(self):
        recipe = (ROOT / "packaging/release/PKGBUILD.in").read_text()
        self.assertIn("CARGO_NET_OFFLINE=true bash scripts/build.sh --release", recipe)
        self.assertIn("cargo build --workspace --locked", (ROOT / "scripts/build.sh").read_text())
        self.assertIn('"$pkgdir/usr/bin/deskwaifu"', recipe)
        self.assertNotIn("private-deps", recipe)
        self.assertNotIn("cmake", recipe)
        workflow = (ROOT / ".gitea/workflows/release.yml").read_text()
        self.assertIn("runs-on: deskwaifu-rust", workflow)
        self.assertNotIn("prune-tests", workflow)
        self.assertNotIn("pacman -", workflow)
        script = (ROOT / "scripts/release-build.sh").read_text()
        self.assertIn("env -i PATH=/usr/bin:/bin", script)
        self.assertNotRegex(script, r"(?m)^\s*sudo\s")


    def test_release_fetch_is_unprivileged_and_shares_offline_build_cache(self):
        script = (ROOT / "scripts/release-build.sh").read_text()
        phase = script[script.index('# Only this dependency-acquisition phase'):script.index('output="$root/release-assets"')]
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
with open({str(log)!r}, "a") as stream:
    stream.write(json.dumps({{"tool": {name!r}, "home": str(home)}}) + "\\n")
''')
                tool.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       build_user="fixture-user", work=str(work), SOURCE_DATE_EPOCH="1700000000",
                       RELEASE_UPLOAD="must-not-leak", GITEA_TOKEN="must-not-leak",
                       RUSTC_WRAPPER="sccache", SCCACHE_DIR="/cache/sccache-rust-package",
                       DESKWAIFU_CI_SCCACHE="1", RELEASE_TOOLS_DIR=str(work),
                       https_proxy="http://proxy.invalid:1234")
            result = subprocess.run(["bash", "-euc", phase], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual([call["tool"] for call in calls], ["cargo", "makepkg"])
            self.assertEqual({call["home"] for call in calls}, {str(work / "home/.cargo")})
            self.assertFalse((work / "fetch-source").exists())
            # A failed dependency fetch must never proceed to compilation.
            log.unlink()
            (tools / "cargo").write_text('#!/bin/sh\nexit 23\n')
            result = subprocess.run(["bash", "-euc", phase], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 23)
            self.assertFalse(log.exists())

    def test_package_build_identity_is_independent_of_pid_and_workspace(self):
        script = (ROOT / "scripts/release-build.sh").read_text()
        self.assertIn("work=/tmp/deskwaifu-package\nmkdir \"$work\"", script)
        self.assertIn("candidate=dwrel\n", script)
        self.assertNotIn("dwrel$$", script)
        self.assertIn("TZ=UTC LANG=C.UTF-8", script)
        self.assertIn('SOURCE_DATE_EPOCH=$(git show -s --format=%ct "$RELEASE_COMMIT")', script)
        self.assertIn('PACKAGER="DeskWaifu reproducible build"', (ROOT / "Containerfile").read_text())


    # 测试目的：确认检出旧标签后仍使用当前工具，且载荷服从旧标签源码而非检出前的分支。
    # 测试方法：在临时 Git 仓库标记单插件版本，再向分支加入语音插件；用保留的工具渲染旧标签配方并检查结果。
    def test_current_tools_survive_checkout_of_historical_tag(self):
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
            (repo / "Cargo.toml").write_text(rust_fixture()["deskwaifu/Cargo.toml"])
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
            self.assertEqual(git("rev-parse", "HEAD"), commit)

    def test_arbitrary_tags_are_used_as_pkgver_without_conversion(self):
        for tag in ("0.1.0", "v0.1.0", "nightly", "release/2026-10", "0.1.0-rc.1", "发布",
                    "0.1.0+build", "@SOURCE_SHA@", "@PLUGINS@", "tag'$(touch SHOULD_NOT_EXIST)"):
            with self.subTest(tag=tag):
                self.assertEqual(source.select_tag("workflow_dispatch", "refs/heads/main", tag), tag)
                self.assertEqual(source.select_tag("push", "refs/tags/" + tag, ""), tag)
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

    def test_tag_selection_never_accepts_branches_or_missing_manual_tag(self):
        self.assertEqual(source.select_tag("push", "refs/tags/v1.2.3", ""), TAG)
        self.assertEqual(source.select_tag("workflow_dispatch", "refs/heads/main", TAG), TAG)
        for event, ref, explicit in [("push", "refs/heads/v1.2.3", ""),
                                     ("workflow_dispatch", "refs/tags/v1.2.3", ""),
                                     ("pull_request", "refs/tags/v1.2.3", "")]:
            with self.assertRaises(ValueError):
                source.select_tag(event, ref, explicit)

    def test_manual_release_requires_notes_before_git_or_build(self):
        for notes in (None, "", " \n\t"):
            with self.subTest(notes=notes):
                env = {"RELEASE_EVENT": "workflow_dispatch", "RELEASE_INPUT_TAG": TAG}
                if notes is not None:
                    env["RELEASE_NOTES"] = notes
                with patch.dict(os.environ, env, clear=True), \
                        patch.object(source, "git", side_effect=AssertionError("git must not run")) as git:
                    with self.assertRaisesRegex(ValueError, "release_notes"):
                        source.main()
                    git.assert_not_called()


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
        self.assertIn("tags: ['**']", workflow)
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
        self.assertIn('bash "$RELEASE_TOOLS_DIR/scripts/release-build.sh"', workflow)
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
            self.assertIn("require root orchestration", result.stderr)
            self.assertNotIn("RELEASE_TAG", result.stderr)

    def test_writable_trusted_checkout_blocks_makepkg(self):
        script = (ROOT / "scripts/release-build.sh").read_text()
        guard = script[script.index('for trusted in '):script.index('output="$root/release-assets"')]
        with tempfile.TemporaryDirectory(prefix="release-trust-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            runuser = work / "runuser"
            runuser.write_text('#!/bin/bash\necho "$*" >> "$CALL_LOG"\nexit 1\n')
            runuser.chmod(0o755)
            env = dict(os.environ, PATH=f"{work}:{os.environ['PATH']}", CALL_LOG=str(work / "calls"),
                       root=str(ROOT), tools_root=str(ROOT), work=str(work), build_user="fixture")
            result = subprocess.run(["bash", "-c", "set -euo pipefail\n" + guard], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("can modify trusted release tooling", result.stderr)
            calls = (work / "calls").read_text()
            self.assertIn("test ! -w", calls)
            self.assertNotIn("makepkg", calls)

    def test_writable_ancestor_blocks_readonly_tool_tree_replacement(self):
        script = (ROOT / "scripts/release-build.sh").read_text()
        guard = script[script.index('for trusted in '):script.index('output="$root/release-assets"')]
        with tempfile.TemporaryDirectory(prefix="release-parent-trust-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            runuser = work / "runuser"
            runuser.write_text('#!/bin/bash\n'
                               'while [[ "$1" != -- ]]; do shift; done\nshift\n'
                               'if [[ "$1 $2" == "test !" ]]; then exit 0; fi\n'
                               'if [[ "$1 $2 $3" == "test -w $WRITABLE_PARENT" ]]; then exit 0; fi\n'
                               'exit 1\n')
            runuser.chmod(0o755)
            env = dict(os.environ, PATH=f"{work}:{os.environ['PATH']}", WRITABLE_PARENT=str(work),
                       root=str(work / "checkout"), tools_root=str(work / "tools"), work=str(work), build_user="fixture")
            result = subprocess.run(["bash", "-c", "set -euo pipefail\n" + guard], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("can replace trusted release tooling", result.stderr)

    def test_root_user_creation_failure_does_not_delete_existing_account(self):
        # Run the actual script's root-user section with failing useradd and
        # mocked privileged commands. No real account operation is performed.
        script = (ROOT / "scripts/release-build.sh").read_text()
        cleanup = script[script.index("build_user=\n"):script.index("mkdir -p")]
        root_block = script[script.index('candidate=dwrel'):script.index('output="$root/release-assets"')]
        with tempfile.TemporaryDirectory(prefix="release-user-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            bin_dir = work / "bin"
            bin_dir.mkdir()
            log = work / "calls"
            for command, body in {
                "id": "echo 0", "useradd": 'echo useradd >> "$CALL_LOG"; exit 9',
                "userdel": 'echo userdel >> "$CALL_LOG"',
                "chown": 'echo chown >> "$CALL_LOG"',
                "runuser": 'echo runuser >> "$CALL_LOG"',
                "rm": 'echo cleanup >> "$CALL_LOG"',
            }.items():
                path = bin_dir / command
                path.write_text("#!/bin/bash\n" + body + "\n")
                path.chmod(0o755)
            env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}", CALL_LOG=str(log),
                       work=str(work), SOURCE_DATE_EPOCH="1700000000")
            result = subprocess.run(["bash", "-c", "set -euo pipefail\n" + cleanup + root_block], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 9, result.stderr)
            self.assertEqual(log.read_text().splitlines(), ["useradd", "cleanup"])


class FakeAPI:
    def __init__(self):
        self.commit = COMMIT
        self.has_commit = True
        self.release = None
        self.assets = {}
        self.calls = []
        self.failure = None
        self.tag_type = "commit"

    def request(self, method, path, data=None, binary=False, upload=False):
        self.calls.append((method, path, data))
        if self.failure and self.failure[:2] == (method, path):
            raise upload_module.ApiError(self.failure[2], path)
        if path.startswith("/git/ref/tags/"):
            if self.commit is None:
                raise upload_module.ApiError(404, path)
            return {"object": {"type": self.tag_type, "sha": self.commit}}
        if path.startswith("/git/tags/"):
            return {"object": {"type": "commit", "sha": self.commit}}
        if path.startswith("/git/commits/"):
            if not self.has_commit:
                raise upload_module.ApiError(404, path)
            return {"sha": COMMIT}
        if path == "/git/refs":
            self.commit = data["sha"]
            return {}
        if path.startswith("/releases/tags/"):
            if self.release is None:
                raise upload_module.ApiError(404, path)
            return self.release
        if path.startswith("/releases?per_page="):
            return [self.release] if self.release else []
        if method == "POST" and path == "/releases":
            self.release = dict(data, id=7)
            return self.release
        if method == "GET" and path.startswith("/releases/7/assets?"):
            return list(self.assets.values())
        if method == "POST" and path.startswith("/releases/7/assets?name="):
            from urllib.parse import unquote
            name = unquote(path.split("name=")[1])
            asset = {"id": len(self.assets) + 1, "name": name, "size": len(data), "state": "uploaded",
                     "digest": "sha256:" + hashlib.sha256(data).hexdigest(), "data": data}
            self.assets[name] = asset
            return asset
        if binary and path.startswith("/releases/assets/"):
            return next(asset["data"] for asset in self.assets.values() if str(asset["id"]) == path.split("/")[-1])
        if method == "PATCH" and path == "/releases/7":
            self.release.update(data)
            return self.release
        raise AssertionError((method, path, data))


# The FakeAPI request's upload keyword must not shadow the imported module.
upload_module = upload


class UploadTest(unittest.TestCase):
    def test_notes_only_never_changes_assets_or_creates_a_release(self):
        api = FakeAPI()
        upload.publish(api, TAG, COMMIT, {"package": b"keep"})
        api.calls.clear()
        upload.update_notes(api, TAG, COMMIT, "新说明\n")
        self.assertEqual(api.assets["package"]["data"], b"keep")
        self.assertEqual([(method, path, data) for method, path, data in api.calls if method != "GET"],
                         [("PATCH", "/releases/7", {"body": "新说明\n"})])
        for state in ("missing", "draft", "wrong-tag", "blank"):
            api = FakeAPI()
            if state == "draft":
                api.release = {"id": 7, "tag_name": TAG, "draft": True}
            if state == "wrong-tag":
                api.commit = OTHER
            with self.subTest(state=state), self.assertRaises(ValueError):
                upload.update_notes(api, TAG, COMMIT, "" if state == "blank" else "说明")
            self.assertTrue(all(method == "GET" for method, _, _ in api.calls))

    def test_notes_only_cli_does_not_require_local_packages(self):
        with patch.dict(os.environ, {"RELEASE_UPLOAD": "TEST", "RELEASE_NOTES": "说明"}, clear=True), \
                patch.object(sys, "argv", ["release-upload.py", "missing", TAG, COMMIT, "--notes-only"]), \
                patch.object(upload, "load_assets", side_effect=AssertionError("must not read packages")), \
                patch.object(upload, "GitHub") as github, patch.object(upload, "update_notes") as update:
            upload.main()
            update.assert_called_once_with(github.return_value, TAG, COMMIT, "说明")

    def test_release_notes_are_preserved_and_identical_rerun_does_not_write(self):
        notes = '## 更新\n- 修复 "窗口" 与 `快捷键`\n$(touch SHOULD_NOT_EXIST)\nEOF\n'
        api = FakeAPI()
        upload.publish(api, TAG, COMMIT, {}, notes)
        self.assertEqual(api.release["body"], notes)
        self.assertFalse(api.release["draft"])
        api.calls.clear()
        upload.publish(api, TAG, COMMIT, {}, notes)
        self.assertTrue(all(method == "GET" for method, _, _ in api.calls))

    def test_notes_update_existing_public_release_or_draft(self):
        for draft in (False, True):
            with self.subTest(draft=draft):
                api = FakeAPI()
                upload.publish(api, TAG, COMMIT, {})
                api.release["draft"] = draft
                api.calls.clear()
                upload.publish(api, TAG, COMMIT, {}, "新说明\n")
                self.assertEqual(api.release["body"], "新说明\n")
                self.assertFalse(api.release["draft"])
                writes = [(method, path, data) for method, path, data in api.calls if method != "GET"]
                expected = {"body": "新说明\n"}
                if draft:
                    expected["draft"] = False
                self.assertEqual(writes, [("PATCH", "/releases/7", expected)])

    def test_absent_or_blank_notes_preserve_existing_body(self):
        api = FakeAPI()
        upload.publish(api, TAG, COMMIT, {})
        self.assertIn(f"Source commit: `{COMMIT}`", api.release["body"])
        api.release["body"] = "人工编辑说明"
        for notes in ("", " \n\t"):
            api.calls.clear()
            upload.publish(api, TAG, COMMIT, {}, notes)
            self.assertEqual(api.release["body"], "人工编辑说明")
            self.assertTrue(all(method == "GET" for method, _, _ in api.calls))

    def test_notes_are_not_updated_when_asset_or_tag_verification_fails(self):
        api = FakeAPI()
        upload.publish(api, TAG, COMMIT, {"a": b"same"}, "原说明")
        with self.assertRaisesRegex(ValueError, "differs"):
            upload.publish(api, TAG, COMMIT, {"a": b"different"}, "新说明")
        self.assertEqual(api.release["body"], "原说明")
        original = api.request

        def move_tag(method, path, data=None, **kwargs):
            result = original(method, path, data, **kwargs)
            if method == "POST" and path.startswith("/releases/7/assets?"):
                api.commit = OTHER
            return result

        api.request = move_tag
        with self.assertRaisesRegex(ValueError, "moved during upload"):
            upload.publish(api, TAG, COMMIT, {"b": b"new"}, "新说明")
        self.assertEqual(api.release["body"], "原说明")

    def test_upload_main_reads_notes_from_environment(self):
        notes = "## 更新\n- 新功能\n"
        with patch.dict(os.environ, {"RELEASE_UPLOAD": "TEST_TOKEN", "RELEASE_NOTES": notes}, clear=True), \
                patch.object(sys, "argv", ["release-upload.py", "assets", TAG, COMMIT]), \
                patch.object(upload, "load_assets", return_value={}), \
                patch.object(upload, "GitHub") as github, patch.object(upload, "publish") as publish:
            upload.main()
            publish.assert_called_once_with(github.return_value, TAG, COMMIT, {}, notes)

    def test_arbitrary_github_release_tags_are_not_renamed_or_classified(self):
        for tag in ("nightly", "release/2026-10", "发布", "0.1.0+build"):
            with self.subTest(tag=tag):
                api = FakeAPI()
                api.commit = None
                upload.publish(api, tag, COMMIT, {})
                self.assertEqual(api.release["tag_name"], tag)
                self.assertEqual(api.release["name"], tag)
                self.assertNotIn("prerelease", api.release)
                self.assertIn(("POST", "/git/refs", {"ref": "refs/tags/" + tag, "sha": COMMIT}), api.calls)

    def test_unprefixed_tag_assets_and_github_release_keep_original_name(self):
        with tempfile.TemporaryDirectory(prefix="release-plain-tag-", dir=TEMP_ROOT) as temporary:
            directory = Path(temporary)
            for name in ("deskwaifu-bin",):
                (directory / f"{name}-0.1.0-1-x86_64.pkg.tar.zst").write_bytes(b"fixture")
            (directory / "release-metadata.json").write_text(json.dumps({
                "tag": "0.1.0", "commit": COMMIT, "pkgver": "0.1.0", "pkgrel": 1}))
            files = sorted(directory.iterdir())
            (directory / "SHA256SUMS").write_text("".join(
                f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in files))
            assets = upload.load_assets(directory, "0.1.0", COMMIT)
            api = FakeAPI()
            api.commit = None
            upload.publish(api, "0.1.0", COMMIT, assets)
            self.assertEqual(api.release["tag_name"], "0.1.0")
            self.assertIn(("POST", "/git/refs", {"ref": "refs/tags/0.1.0", "sha": COMMIT}), api.calls)
            with self.assertRaisesRegex(ValueError, "artifacts"):
                upload.load_assets(directory, "v0.1.0", COMMIT)

    def test_stdlib_uploader_honors_https_proxy_without_network(self):
        proxy = "http://100.64.0.11:7912"
        response = addinfourl(io.BytesIO(b'{"ok": true}'), {}, upload.API + "/releases", 200)
        response.msg = "OK"
        with patch.dict(os.environ, {"http_proxy": proxy, "https_proxy": proxy, "ALL_PROXY": proxy,
                                     "no_proxy": "127.0.0.1,localhost"}, clear=True):
            with patch("urllib.request.ProxyHandler.proxy_open", autospec=True, return_value=response) as proxied:
                module = load("release-upload")
                self.assertEqual(module.GitHub("TEST_TOKEN").request("GET", "/releases"), {"ok": True})
                proxied.assert_called_once()
                self.assertEqual(proxied.call_args.args[2:], (proxy, "https"))

    def test_create_release_and_identical_rerun_make_no_writes(self):
        api = FakeAPI()
        api.commit = None
        assets = {"one.pkg.tar.zst": b"one", "two.pkg.tar.zst": b"two"}
        upload.publish(api, TAG, COMMIT, assets)
        self.assertEqual(api.commit, COMMIT)
        self.assertFalse(api.release["draft"])
        api.calls.clear()
        upload.publish(api, TAG, COMMIT, assets)
        self.assertTrue(all(method == "GET" for method, _, _ in api.calls))

    def test_annotated_tag_and_legacy_digest_download(self):
        api = FakeAPI()
        api.tag_type = "tag"
        upload.publish(api, TAG, COMMIT, {"a": b"same"})
        del api.assets["a"]["digest"]
        upload.publish(api, TAG, COMMIT, {"a": b"same"})
        self.assertTrue(any(path == "/releases/assets/1" for _, path, _ in api.calls))

    def test_differing_assets_fail_preflight_without_uploading_missing_assets(self):
        api = FakeAPI()
        upload.publish(api, TAG, COMMIT, {"old": b"old"})
        api.calls.clear()
        with self.assertRaisesRegex(ValueError, "differs"):
            upload.publish(api, TAG, COMMIT, {"new": b"new", "old": b"BAD"})
        self.assertTrue(all(method == "GET" for method, _, _ in api.calls))
        self.assertNotIn("new", api.assets)

    def test_missing_commit_and_wrong_tag_never_create_release(self):
        api = FakeAPI()
        api.commit = OTHER
        with self.assertRaisesRegex(ValueError, "different commit"):
            upload.publish(api, TAG, COMMIT, {})
        self.assertTrue(all(method == "GET" for method, _, _ in api.calls))
        api.commit = None
        api.has_commit = False
        with self.assertRaises(upload.ApiError):
            upload.publish(api, TAG, COMMIT, {})
        self.assertIsNone(api.release)

    def test_api_failure_is_not_treated_as_not_found(self):
        for status in (401, 403, 429, 500):
            with self.subTest(status=status):
                api = FakeAPI()
                api.failure = ("GET", "/releases/tags/" + TAG, status)
                with self.assertRaises(upload.ApiError) as error:
                    upload.publish(api, TAG, COMMIT, {})
                self.assertEqual(error.exception.status, status)
                self.assertIsNone(api.release)

    def test_upload_failure_leaves_draft_for_safe_retry(self):
        api = FakeAPI()
        api.failure = ("POST", "/releases/7/assets?name=a", 500)
        with self.assertRaises(upload.ApiError):
            upload.publish(api, TAG, COMMIT, {"a": b"a"})
        self.assertTrue(api.release["draft"])
        api.failure = None
        upload.publish(api, TAG, COMMIT, {"a": b"a"})
        self.assertFalse(api.release["draft"])

    def test_draft_hidden_by_tag_endpoint_is_reused(self):
        api = FakeAPI()
        upload.publish(api, TAG, COMMIT, {"a": b"a"})
        api.release["draft"] = True
        api.failure = ("GET", "/releases/tags/" + TAG, 404)
        api.calls.clear()
        upload.publish(api, TAG, COMMIT, {"a": b"a"})
        self.assertFalse(api.release["draft"])
        self.assertFalse(any(method == "POST" for method, _, _ in api.calls))

    def test_redirect_drops_token_on_cdn_and_rejects_plaintext(self):
        request = upload.Request(upload.API + "/releases/assets/1", headers={"Authorization": "Bearer SECRET"})
        redirect = upload.SafeRedirect().redirect_request(
            request, None, 302, "Found", {}, "https://release-assets.githubusercontent.com/file")
        self.assertFalse(redirect.has_header("Authorization"))
        with self.assertRaisesRegex(ValueError, "non-HTTPS"):
            upload.SafeRedirect().redirect_request(request, None, 302, "Found", {}, "http://example.invalid/file")

    def test_upload_conflict_accepts_only_identical_concurrent_winner(self):
        for winning_data in (b"same", b"DIFF"):
            with self.subTest(winner=winning_data):
                api = FakeAPI()
                original = api.request

                def race(method, path, data=None, **kwargs):
                    if method == "POST" and path == "/releases/7/assets?name=a":
                        original(method, path, winning_data, **kwargs)
                        raise upload.ApiError(422, path)
                    return original(method, path, data, **kwargs)

                api.request = race
                if winning_data == b"same":
                    upload.publish(api, TAG, COMMIT, {"a": b"same"})
                    self.assertFalse(api.release["draft"])
                else:
                    with self.assertRaisesRegex(ValueError, "differs"):
                        upload.publish(api, TAG, COMMIT, {"a": b"same"})
                    self.assertTrue(api.release["draft"])

    def test_tag_move_during_upload_does_not_publish_draft(self):
        api = FakeAPI()
        original = api.request

        def move_tag(method, path, data=None, **kwargs):
            result = original(method, path, data, **kwargs)
            if method == "POST" and path.startswith("/releases/7/assets?"):
                api.commit = OTHER
            return result

        api.request = move_tag
        with self.assertRaisesRegex(ValueError, "moved during upload"):
            upload.publish(api, TAG, COMMIT, {"a": b"same"})
        self.assertTrue(api.release["draft"])

    def test_incomplete_remote_asset_never_overwritten(self):
        api = FakeAPI()
        upload.publish(api, TAG, COMMIT, {"a": b"a"})
        api.assets["a"]["state"] = "starter"
        with self.assertRaisesRegex(ValueError, "incomplete"):
            upload.publish(api, TAG, COMMIT, {"a": b"a"})

    def test_stdlib_http_errors_hide_token(self):
        with patch.object(upload, "urlopen", side_effect=HTTPError("url", 403, "denied", {}, None)):
            with self.assertRaises(upload.ApiError) as error:
                upload.GitHub("TOP_SECRET").request("GET", "/releases")
        self.assertNotIn("TOP_SECRET", str(error.exception))


if __name__ == "__main__":
    unittest.main()
