import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parent.parent
COMMIT = "b" * 40
PKGVER = "0.0.0.r" + COMMIT
PACKAGE = f"deskwaifu-bin-{PKGVER}-1-x86_64.pkg.tar.zst"


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


nightly = load("release-nightly")


class _NoContentHandler(http.server.BaseHTTPRequestHandler):
    def do_DELETE(self):
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


def _not_found(method=None):
    raise nightly.upload_module().ApiError(404, "/missing")


class FakeApi:
    """记录请求并按脚本返回响应的最小 GitHub API 替身。"""

    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def request(self, method, path, data=None, binary=False, upload=False):
        self.calls.append((method, path, data))
        key = (method, path.split("?")[0])
        if key not in self.responses:
            raise AssertionError(f"unexpected request {method} {path}")
        value = self.responses[key]
        return value() if callable(value) else value


def write_assets(directory, *, tamper=False):
    package = directory / PACKAGE
    package.write_bytes(b"package bytes")
    metadata = {"tag": PKGVER, "commit": COMMIT, "pkgver": PKGVER, "pkgrel": 1}
    (directory / "release-metadata.json").write_text(json.dumps(metadata, sort_keys=True, indent=2) + "\n")
    lines = []
    for name in (PACKAGE, "release-metadata.json"):
        lines.append(f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n")
    (directory / "SHA256SUMS").write_text("".join(lines))
    if tamper:
        package.write_bytes(b"tampered")


class NightlyTest(unittest.TestCase):
    # 测试目的：nightly 预发布只保留本次构建的资产，同名资产在字节变化时替换、在重试时复用。
    # 测试方法：用假 API 走一次发布，检查删除旧资产、上传新包和发布为预发布。
    def test_publish_replaces_previous_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_assets(directory)
            assets = nightly.expected_assets(directory)
            existing = [
                {"id": 1, "name": "deskwaifu-bin-0.0.0.rold-1-x86_64.pkg.tar.zst"},
                {"id": 2, "name": "SHA256SUMS", "digest": "sha256:" + "0" * 64, "size": 1},
                {"id": 3, "name": "stray.txt"},
            ]
            api = FakeApi({
                ("GET", "/git/refs/tags/nightly"): {"object": {"sha": COMMIT}},
                ("GET", "/releases/tags/nightly"): {"id": 7, "tag_name": "nightly", "draft": False},
                ("GET", "/releases/7/assets"): existing,
                # GitHub 删除成功返回 204，没有响应体。
                ("DELETE", "/releases/assets/1"): None,
                ("DELETE", "/releases/assets/2"): None,
                ("DELETE", "/releases/assets/3"): None,
                ("POST", "/releases/7/assets"): lambda: {"digest": None},
                ("PATCH", "/releases/7"): {},
            })
            nightly.publish(api, COMMIT, assets, nightly.release_body(COMMIT, "https://git.example", "2026-11-26 00:00 UTC"))
            deleted = {path for method, path, _ in api.calls if method == "DELETE"}
            self.assertEqual(deleted, {"/releases/assets/1", "/releases/assets/2",
                                       "/releases/assets/3"})
            # GitHub 只提供 /releases/assets/{id}，带 release id 的删除路由会返回 404。
            self.assertFalse([path for path in deleted if path.startswith("/releases/7/")])
            uploaded = [payload for method, _, payload in api.calls
                        if method == "POST" and isinstance(payload, bytes)]
            self.assertEqual(len(uploaded), 3)
            patch = next(data for method, path, data in api.calls
                         if method == "PATCH" and path == "/releases/7")
            self.assertTrue(patch["prerelease"])

    # 测试目的：nightly 标签一旦存在就不再移动，提交身份只由发布正文和资产名携带。
    # 测试方法：标签已存在时检查只读取标签，不存在时才创建指向本次提交的标签。
    def test_existing_tag_is_reused(self):
        existing = FakeApi({("GET", "/git/refs/tags/nightly"): {"object": {"sha": "c" * 40}}})
        nightly.ensure_github_tag(existing, COMMIT)
        self.assertEqual([method for method, _, _ in existing.calls], ["GET"])

        missing = FakeApi({
            ("GET", "/git/refs/tags/nightly"): _not_found,
            ("GET", "/git/commits/" + COMMIT): {"sha": COMMIT},
            ("POST", "/git/refs"): {},
        })
        nightly.ensure_github_tag(missing, COMMIT)
        created = [data for method, path, data in missing.calls if method == "POST"]
        self.assertEqual(created, [{"ref": "refs/tags/nightly", "sha": COMMIT}])

    # 测试目的：删除成功时 GitHub 返回 204 无响应体，客户端不能把它当成解析失败。
    # 测试方法：用真实客户端请求本地 204 端点，确认返回空值而不是抛异常。
    def test_empty_response_body_is_not_a_parse_error(self):
        upload = nightly.upload_module()
        server = http.server.HTTPServer(("127.0.0.1", 0), _NoContentHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            api = upload.GitHub("token")
            upload.API = f"http://127.0.0.1:{server.server_port}"
            self.assertIsNone(api.request("DELETE", "/releases/assets/1"))
        finally:
            server.shutdown()
            server.server_close()

    # 测试目的：产物摘要不符时不上传，避免把损坏的包发成 nightly。
    # 测试方法：篡改包内容后要求校验失败。
    def test_checksum_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_assets(directory, tamper=True)
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                nightly.expected_assets(directory)

    # 测试目的：nightly 标签必须先在 Gitea 中存在，push mirror 才不会删掉它。
    # 测试方法：替换 Gitea 调用，已有标签时不再写入，缺失时按本次提交创建。
    def test_gitea_tag_is_created_once(self):
        environment = dict(os.environ, GITEA_API_URL="https://gitea.example/api/v1",
                           GITHUB_REPOSITORY="owner/repo")

        def run(existing):
            calls = []
            original = nightly.gitea_call
            nightly.gitea_call = lambda method, url, data=None: calls.append((method, url, data)) or (
                existing if method == "GET" else {})
            try:
                with unittest.mock.patch.dict(os.environ, environment, clear=False):
                    nightly.tag_gitea(COMMIT)
            finally:
                nightly.gitea_call = original
            return calls

        present = run([{"name": "nightly", "commit": {"sha": "d" * 40}}])
        self.assertEqual([method for method, _, _ in present], ["GET"])

        missing = run([])
        self.assertEqual([method for method, _, _ in missing], ["GET", "POST"])
        self.assertEqual(missing[-1][2], {"tag_name": "nightly", "target": COMMIT})

    # 测试目的：合并到 main 只构建并发布，不再跑安装包验收。
    # 测试方法：读取 nightly 工作流，检查触发条件、作业数和不引用验收入口。
    def test_nightly_workflow_builds_and_publishes_only(self):
        workflow = (ROOT / ".gitea/workflows/nightly.yml").read_text()
        self.assertIn("branches: [main]", workflow)
        self.assertIn("runs-on: deskwaifu-rust", workflow)
        self.assertIn("scripts/release-nightly.py tag", workflow)
        self.assertIn("scripts/release-nightly.py publish", workflow)
        self.assertIn("bash scripts/release-build.sh", workflow)
        self.assertNotIn("run_suite.py", workflow)
        self.assertNotIn("deskwaifu-gui", workflow)
        self.assertEqual(workflow.count("secrets.RELEASE_UPLOAD"), 1)
        self.assertNotIn("secrets.RELEASE_UPLOAD",
                         workflow.split("- name: Publish nightly release")[0])
        ci = (ROOT / ".gitea/workflows/ci.yml").read_text()
        self.assertNotIn("branches: [main]", ci)


if __name__ == "__main__":
    unittest.main()
