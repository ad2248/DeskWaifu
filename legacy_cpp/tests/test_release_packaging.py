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
        return {"usr/lib/deskwaifu/ocr/deskwaifu-ocr.so"}
    return {"usr/bin/deskwaifu", "usr/bin/deskwaifu-control", "usr/bin/deskwaifu-ctl", "usr/bin/deskwaifu-cursor-guard",
            *(f"usr/lib/deskwaifu/plugins/{name}/{name}.so" for name in packaging.PLUGINS),
            *packaging.SDK_LIBRARIES}


class PackagingTest(unittest.TestCase):
    def release_source_command(self):
        # Archived workflows are absent from the source tree. Supply only their
        # contents as fixture data and run the real release script against Git.
        temporary = tempfile.TemporaryDirectory(prefix="release-tool-fixture-")
        self.addCleanup(temporary.cleanup)
        tools = Path(temporary.name)
        for directory in ("scripts", "packaging/release", "cmake"):
            shutil.copytree(ROOT / directory, tools / directory,
                            ignore=shutil.ignore_patterns("__pycache__"))
        for relative in ("Containerfile", "tests/test_release_packaging.py", "tests/gate.json",
                         "plugins/perception/tests/test_ocr_native_models.py"):
            target = tools / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        workflows = tools / ".gitea/workflows"
        workflows.mkdir(parents=True)
        for name in ("ci", "release"):
            (workflows / f"{name}.yml").write_text(f"name: fixture-{name}\n")
        return [sys.executable, "-B", str(tools / "scripts/release-source.py")]

    def test_release_build_uses_persistent_cache_only_when_mounted_and_not_clean(self):
        script = (ROOT / "scripts/release-build.sh").read_text()
        self.assertIn('if [[ "${DESKWAIFU_CLEAN_BUILD:-false}" != "true" && -d /cache && -x /usr/bin/ccache ]]', script)
        self.assertIn("/etc/makepkg.conf.d/zz-deskwaifu-ccache.conf", script)
        self.assertIn("export DESKWAIFU_CCACHE=1", script)
        self.assertIn("export CCACHE_DIR=/cache/ccache-makepkg", script)
        self.assertIn('chown "$build_user:$build_user" /cache/ccache-makepkg', script)
        pkgbuild = (ROOT / "packaging/release/PKGBUILD.in").read_text()
        self.assertEqual(pkgbuild.count('if [[ -n "${DESKWAIFU_CCACHE:-}" ]]; then'), 1)
        self.assertEqual(pkgbuild.count("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"), 1,
                         "The launcher array must feed both the main and OCR configures")
        self.assertIn("ccache \\\n", (ROOT / "Containerfile").read_text())

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
                       RELEASE_INPUT_TAG="0.1.0", RELEASE_NOTES="## 更新\n- 历史版本发布\n",
                       GITHUB_ENV=str(env_file), RUNNER_TEMP=str(root))
            result = subprocess.run(self.release_source_command(),
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
            (acquired / "release-tools.tar.gz").write_bytes(b"tools")
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
                    (work / "release-tools.tar.gz").write_bytes(b"tools")
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
            (work / "release-tools.tar.gz").write_bytes(b"tools")
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
            command = self.release_source_command()
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
            result = subprocess.run(self.release_source_command(),
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
            result = subprocess.run(self.release_source_command(),
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
            command = self.release_source_command()
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
            result = subprocess.run(self.release_source_command(),
                                    cwd=checkout, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(git("rev-parse", tag), head)

    @unittest.skipUnless(shutil.which("makepkg") and shutil.which("bsdtar") and shutil.which("fakeroot")
                         and os.getuid() != 0, "requires Arch makepkg/fakeroot and a nonroot user")
    def test_real_makepkg_split_package_metadata_and_payload(self):
        # Exercise the real PKGBUILD package functions with an offline fake CMake
        # installer. This is a packaging test, NOT an ELF/real application build.
        with tempfile.TemporaryDirectory(prefix="release-packaging-", dir=TEMP_ROOT) as temporary:
            work = Path(temporary)
            archive(work / "deskwaifu.tar.gz", {
                "deskwaifu/README.md": "fixture",
                "deskwaifu/plugins/perception/native/CMakeLists.txt": "# fixture"})
            archive(work / "private-deps.tar.gz", {
                "private-deps/CubismSdkForNative-5-r.5/LICENSE.md": "fixture",
                "private-deps/CubismSdkForNative-5-r.5/Core/LICENSE.md": "fixture",
                "private-deps/CubismSdkForNative-5-r.5/Framework/LICENSE.md": "fixture",
                "private-deps/glew/LICENSE.txt": "fixture"})
            archive(work / "release-tools.tar.gz", {
                "release-tools/scripts/test-environment.py": "# fixture\n",
                "release-tools/scripts/prepare-installed-tests.py":
                    "import pathlib,sys\np=pathlib.Path(sys.argv[1]);p.mkdir(exist_ok=True)\n"
                    "(p/'installed-scripts').mkdir(exist_ok=True)\n"})
            packaging.prepare(work, TAG, COMMIT)
            pkgbuild = work / "PKGBUILD"
            subprocess.run(["bash", "-n", str(pkgbuild)], check=True)
            srcinfo = subprocess.check_output(["makepkg", "--printsrcinfo"], cwd=work, text=True)
            sections = srcinfo.split("\npkgname = ")[1:]
            for section in sections:
                dependencies = packaging.metadata("\n".join(line.strip() for line in section.splitlines())).get("depends", [])
                packaging.verify_unversioned_dependencies(dependencies)
                name = section.splitlines()[0]
                if name in ("deskwaifu-ocr-bin", "deskwaifu-tests"):
                    self.assertIn("deskwaifu-bin", dependencies)
                elif name == "deskwaifu-ocr-tests":
                    self.assertIn("deskwaifu-ocr-bin", dependencies)
            self.assertIn("depends = yaml-cpp", srcinfo)
            self.assertIn("depends = onnxruntime-cpu", srcinfo)
            self.assertIn("makedepends = onnxruntime-cpu", srcinfo)
            self.assertNotIn("onnxruntime-linux", srcinfo)
            self.assertIn("makedepends = opencv", srcinfo)
            self.assertEqual(srcinfo.count("pkgname = "), 4)
            bin_dir = work / "bin"
            bin_dir.mkdir()
            fake = bin_dir / "cmake"
            fake.write_text("#!/usr/bin/env python3\nimport os, pathlib, sys\n"
                            "if '--install' not in sys.argv: sys.exit(0)\n"
                            f"files = {sorted(fixture_files())!r} if 'build-main' in sys.argv[2] else {sorted(fixture_files(True))!r}\n"
                            "if sys.argv[-1] in ('DeskWaifuTests','DeskWaifuOCRTests'):\n"
                            " prefix='usr/lib/deskwaifu/' + ('ocr-tests' if sys.argv[-1]=='DeskWaifuOCRTests' else 'tests')\n"
                            " files=[prefix+'/run.py',prefix+'/installed-tests.json']\n"
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
            packaging.verify(work / "packages", TAG, COMMIT, include_tests=True)
            first_build = {path.name: path.read_bytes() for path in (work / "packages").glob("*.pkg.tar.zst")}
            result = subprocess.run(["makepkg", "--nodeps", "--cleanbuild", "--force", "--noconfirm"],
                                    cwd=work, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(first_build, {path.name: path.read_bytes()
                                          for path in (work / "packages").glob("*.pkg.tar.zst")})
            packaging.prune_tests(work / "packages", TAG, COMMIT)
            assets = upload.load_assets(work / "packages", TAG, COMMIT)
            self.assertEqual(len(assets), 4)
            package = next((work / "packages").glob("deskwaifu-ocr-bin-*.pkg.tar.zst"))
            package.write_bytes(package.read_bytes() + b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                upload.load_assets(work / "packages", TAG, COMMIT)

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

if __name__ == "__main__":
    unittest.main()
