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


def fixture_files(ocr=False):
    if ocr:
        return {"usr/libexec/deskwaifu-ocr", "usr/lib/deskwaifu/ocr/libonnxruntime.so.1"}
    return {"usr/bin/deskwaifu", "usr/bin/deskwaifu-control", "usr/bin/deskwaifu-ctl", "usr/bin/deskwaifu-cursor-guard",
            *(f"usr/lib/deskwaifu/plugins/{name}/{name}.so" for name in packaging.PLUGINS),
            *packaging.SDK_LIBRARIES}


class PackagingTest(unittest.TestCase):
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
            git("add", ".")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "historical")
            commit = git("rev-parse", "HEAD")
            git("tag", "0.1.0")
            historical.write_text("# newer source\n")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-am", "later")
            env_file = root / "env"
            env = dict(os.environ, RELEASE_EVENT="workflow_dispatch", RELEASE_REF="refs/heads/main",
                       RELEASE_INPUT_TAG="0.1.0", GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/release-source.py")],
                                    cwd=repo, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            values = dict(line.split("=", 1) for line in env_file.read_text().splitlines())
            tools = Path(values["RELEASE_TOOLS_DIR"])
            self.assertFalse(tools.is_relative_to(repo))
            self.assertIn("historical packaging must not run", historical.read_text())
            acquired = root / "acquired"
            acquired.mkdir()
            (acquired / "deskwaifu.tar.gz").write_bytes(b"source")
            (acquired / "private-deps.tar.gz").write_bytes(b"deps")
            subprocess.run([sys.executable, "-B", str(tools / "scripts/release-packaging.py"),
                            "prepare", str(acquired), "0.1.0", commit], check=True)
            self.assertIn("pkgver=0.1.0\n", (acquired / "PKGBUILD").read_text())
            self.assertEqual(git("rev-parse", "HEAD"), commit)

    def test_arbitrary_tags_are_used_as_pkgver_without_conversion(self):
        for tag in ("0.1.0", "v0.1.0", "nightly", "release/2026-10", "0.1.0-rc.1", "发布",
                    "0.1.0+build", "@SOURCE_SHA@", "tag'$(touch SHOULD_NOT_EXIST)"):
            with self.subTest(tag=tag):
                self.assertEqual(source.select_tag("workflow_dispatch", "refs/heads/main", tag), tag)
                self.assertEqual(source.select_tag("push", "refs/tags/" + tag, ""), tag)
                with tempfile.TemporaryDirectory(prefix="release-raw-tag-", dir=TEMP_ROOT) as temporary:
                    work = Path(temporary)
                    (work / "deskwaifu.tar.gz").write_bytes(b"source")
                    (work / "private-deps.tar.gz").write_bytes(b"deps")
                    packaging.prepare(work, tag, COMMIT)
                    result = subprocess.check_output(["bash", "-c", '. "$1"; printf "%s" "$pkgver"',
                                                      "_", str(work / "PKGBUILD")], cwd=work, text=True)
                    self.assertEqual(result, tag)
                    self.assertFalse((work / "SHOULD_NOT_EXIST").exists())

    @unittest.skipUnless(shutil.which("makepkg") and os.getuid() != 0, "requires nonroot makepkg")
    def test_arch_incompatible_tags_fail_in_makepkg_without_fallback(self):
        with tempfile.TemporaryDirectory(prefix="release-invalid-pkgver-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            (work / "deskwaifu.tar.gz").write_bytes(b"source")
            (work / "private-deps.tar.gz").write_bytes(b"deps")
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

    def test_payload_and_exact_dependency(self):
        for name, ocr in (("deskwaifu-bin", False), ("deskwaifu-ocr-bin", True)):
            info = {"pkgname": [name], "pkgver": ["1.2.3-1"], "arch": ["x86_64"],
                    "depend": ["deskwaifu-bin=1.2.3-1", "opencv", "yaml-cpp"] if ocr else sorted(packaging.MAIN_DEPENDS)}
            packaging.verify_content(name, info, fixture_files(ocr), "1.2.3")
            for extra in ("usr/share/deskwaifu/ocr/detection.onnx", "usr/share/other/dictionary.json", "usr/share/model/inference.yml"):
                with self.assertRaisesRegex(ValueError, "Models/dictionaries"):
                    packaging.verify_content(name, info, fixture_files(ocr) | {extra}, "1.2.3")
            with self.assertRaisesRegex(ValueError, "Missing package files"):
                packaging.verify_content(name, info, set(), "1.2.3")
        info["depend"] = ["deskwaifu-bin>=1.2.3", "opencv", "yaml-cpp"]
        with self.assertRaisesRegex(ValueError, "exact main package"):
            packaging.verify_content(name, info, fixture_files(True), "1.2.3")
        info["depend"] = ["deskwaifu-bin=1.2.3-1", "opencv"]
        with self.assertRaisesRegex(ValueError, "yaml-cpp"):
            packaging.verify_content(name, info, fixture_files(True), "1.2.3")

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
                       RELEASE_INPUT_TAG=TAG, GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
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

    def test_package_split_does_not_leak_runtime_or_plugins(self):
        main_info = {"pkgname": ["deskwaifu-bin"], "pkgver": ["1.2.3-1"], "arch": ["x86_64"]}
        with self.assertRaisesRegex(ValueError, "must not contain OCR"):
            packaging.verify_content("deskwaifu-bin", main_info, fixture_files() | fixture_files(True), "1.2.3")
        main_info["depend"] = ["opencv"]
        with self.assertRaisesRegex(ValueError, "must not require"):
            packaging.verify_content("deskwaifu-bin", main_info, fixture_files(), "1.2.3")

    def test_main_requires_direct_dependencies_and_all_sdk_runtime_libraries(self):
        info = {"pkgname": ["deskwaifu-bin"], "pkgver": ["1.2.3-1"], "arch": ["x86_64"],
                "depend": sorted(packaging.MAIN_DEPENDS)}
        for missing in packaging.MAIN_DEPENDS:
            with self.subTest(dependency=missing), self.assertRaisesRegex(ValueError, "Missing main runtime dependencies"):
                packaging.verify_content("deskwaifu-bin", dict(info, depend=sorted(packaging.MAIN_DEPENDS - {missing})),
                                         fixture_files(), "1.2.3")
        with self.assertRaisesRegex(ValueError, "Missing main runtime dependencies"):
            packaging.verify_content("deskwaifu-bin", dict(info, depend=[]), fixture_files(), "1.2.3")
        for missing in packaging.SDK_LIBRARIES:
            with self.subTest(library=missing), self.assertRaisesRegex(ValueError, "Missing package files"):
                packaging.verify_content("deskwaifu-bin", info, fixture_files() - {missing}, "1.2.3")

    @unittest.skipUnless(shutil.which("makepkg") and shutil.which("bsdtar") and shutil.which("fakeroot")
                         and os.getuid() != 0, "requires Arch makepkg/fakeroot and a nonroot user")
    def test_real_makepkg_split_package_metadata_and_payload(self):
        # Exercise the real PKGBUILD package functions with an offline fake CMake
        # installer. This is a packaging test, NOT an ELF/real application build.
        with tempfile.TemporaryDirectory(prefix="release-packaging-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            archive(work / "deskwaifu.tar.gz", {"deskwaifu/README.md": "fixture"})
            archive(work / "private-deps.tar.gz", {
                "private-deps/CubismSdkForNative-5-r.5/LICENSE.md": "fixture",
                "private-deps/CubismSdkForNative-5-r.5/Core/LICENSE.md": "fixture",
                "private-deps/CubismSdkForNative-5-r.5/Framework/LICENSE.md": "fixture",
                "private-deps/glew/LICENSE.txt": "fixture"})
            sdk = work / "onnxruntime-linux-x64-1.31.0.tgz"
            archive(sdk, {"onnxruntime-linux-x64-1.31.0/LICENSE": "fixture"})
            packaging.prepare(work, TAG, COMMIT)
            pkgbuild = work / "PKGBUILD"
            # Only the fixture SDK checksum is substituted. Production remains
            # pinned to the official SDK and makepkg verifies every fixture hash.
            pkgbuild.write_text(pkgbuild.read_text().replace(
                "cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57", packaging.digest(sdk)))
            subprocess.run(["bash", "-n", str(pkgbuild)], check=True)
            srcinfo = subprocess.check_output(["makepkg", "--printsrcinfo"], cwd=work, text=True)
            self.assertIn(f"depends = deskwaifu-bin={TAG}-1", srcinfo)
            self.assertIn("depends = yaml-cpp", srcinfo)
            self.assertIn("makedepends = opencv", srcinfo)
            self.assertEqual(srcinfo.count("pkgname = "), 2)
            bin_dir = work / "bin"
            bin_dir.mkdir()
            fake = bin_dir / "cmake"
            fake.write_text("#!/usr/bin/env python3\nimport os, pathlib, sys\n"
                            "if '--install' not in sys.argv: sys.exit(0)\n"
                            f"files = {sorted(fixture_files())!r} if 'build-main' in sys.argv[2] else {sorted(fixture_files(True))!r}\n"
                            "for name in files:\n"
                            " p = pathlib.Path(os.environ['DESTDIR']) / name\n"
                            " p.parent.mkdir(parents=True, exist_ok=True)\n"
                            " p.write_text('fixture, not an ELF binary\\n')\n")
            fake.chmod(0o755)
            env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}", PKGDEST=str(work / "packages"),
                       SOURCE_DATE_EPOCH="1700000000")
            result = subprocess.run(["makepkg", "--nodeps", "--noconfirm"], cwd=work, env=env,
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            packaging.verify(work / "packages", TAG, COMMIT)
            assets = upload.load_assets(work / "packages", TAG, COMMIT)
            self.assertEqual(len(assets), 4)
            package = next((work / "packages").glob("deskwaifu-ocr-bin-*.pkg.tar.zst"))
            package.write_bytes(package.read_bytes() + b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                upload.load_assets(work / "packages", TAG, COMMIT)

    def test_workflow_secret_is_scoped_to_final_step(self):
        workflow = (ROOT / ".gitea/workflows/release.yml").read_text()
        self.assertEqual(workflow.count("secrets.RELEASE_UPLOAD"), 1)
        self.assertNotIn("secrets.RELEASE_UPLOAD", workflow.split("- name: Upload verified packages")[0])
        self.assertIn("runs-on: deskwaifu-arch", workflow)
        self.assertIn("tags: ['**']", workflow)
        self.assertIn("https://git.kals.top/actions/checkout@v4", workflow)
        self.assertIn("RELEASE_INPUT_TAG: ${{ github.event.inputs.tag }}", workflow)
        self.assertIn('python3 -B "$RELEASE_TOOLS_DIR/tests/test_release_packaging.py"', workflow)
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
        root_block = script[script.index('candidate="dwrel'):script.index('output="$root/release-assets"')]
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
            for name in ("deskwaifu-bin", "deskwaifu-ocr-bin"):
                (directory / f"{name}-0.1.0-1-x86_64.pkg.tar.zst").write_bytes(b"fixture")
            (directory / "release-metadata.json").write_text(json.dumps({
                "tag": "0.1.0", "commit": COMMIT, "pkgver": "0.1.0", "pkgrel": 1, "ocr_models_included": False}))
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
