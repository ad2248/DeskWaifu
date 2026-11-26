#!/usr/bin/env python3
"""Publish the package built from each main commit as the GitHub nightly release.

`tag` 在 Gitea 中创建 `nightly` 标签。Gitea 的 push mirror 会删除 GitHub 上
多出来的标签，所以标签必须先存在于 Gitea。标签只在首次发布时创建，之后不再
移动：提交身份由发布正文和资产文件名携带，避免与强制镜像互相覆盖。
`publish` 把 `nightly` 预发布换成这次的包。
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent.parent
NIGHTLY_TAG = "nightly"
NIGHTLY_NAME = "nightly"
PACKAGE_PATTERN = re.compile(r"deskwaifu-bin-(?P<pkgver>.+)-1-x86_64\.pkg\.tar\.zst")
FIXED_ASSETS = ("release-metadata.json", "SHA256SUMS")


_UPLOAD = None


def upload_module():
    """加载 release-upload 一次，复用同一份 GitHub 客户端与异常类型。"""
    global _UPLOAD
    if _UPLOAD is None:
        spec = importlib.util.spec_from_file_location("release_upload", ROOT / "scripts" / "release-upload.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _UPLOAD = module
    return _UPLOAD


def expected_assets(directory):
    """校验本次构建的产物，返回按资产名索引的内容。"""
    packages = sorted(directory.glob("*.pkg.tar.zst"))
    if len(packages) != 1:
        raise ValueError("Expected exactly one package for the nightly release")
    match = PACKAGE_PATTERN.fullmatch(packages[0].name)
    if match is None:
        raise ValueError("Unexpected nightly package name: " + packages[0].name)
    names = {packages[0].name, *FIXED_ASSETS}
    if {path.name for path in directory.iterdir()} != names:
        raise ValueError("Unexpected or missing nightly artifacts")
    assets = {name: (directory / name).read_bytes() for name in sorted(names)}
    manifest = json.loads(assets["release-metadata.json"])
    pkgver = match.group("pkgver")
    if manifest.get("pkgver") != pkgver or manifest.get("tag") != pkgver:
        raise ValueError("Nightly manifest does not match the package version")
    if not re.fullmatch(r"[0-9a-f]{40}", str(manifest.get("commit", ""))):
        raise ValueError("Nightly manifest lacks a full source commit")
    sums = "".join(f"{hashlib.sha256(assets[name]).hexdigest()}  {name}\n"
                   for name in sorted(names - {"SHA256SUMS"}))
    if assets["SHA256SUMS"].decode() != sums:
        raise ValueError("Nightly artifact checksum mismatch")
    return assets


def stale_assets(existing, current):
    """预发布里只保留本次构建的资产。"""
    return sorted(name for name in existing if name not in current)


def release_body(commit, server, built_at):
    return ("每次 `main` 构建都会覆盖此预发布。\n\n"
            "- 源码提交：{server}/commit/{commit}\n"
            "- 构建时间：{built_at}\n"
            "- 安装包：`deskwaifu-bin-0.0.0.r{commit}-1-x86_64.pkg.tar.zst`\n\n"
            "这是滚动构建，不保证可复现或兼容。安装后在终端运行 `deskwaifu`。").format(
                server=server.rstrip("/"), commit=commit, built_at=built_at)


def wait_for_commit(api, commit, timeout=180.0, interval=5.0):
    """等待 push mirror 把本次提交同步到 GitHub。"""
    ApiError = upload_module().ApiError
    deadline = time.monotonic() + timeout
    while True:
        try:
            if api.request("GET", "/git/commits/" + commit)["sha"] == commit:
                return
        except ApiError as error:
            if error.status != 404:
                raise
        if time.monotonic() >= deadline:
            raise ValueError("GitHub does not have the nightly source commit")
        time.sleep(interval)


def ensure_github_tag(api, commit):
    """确保 GitHub 存在 nightly 标签；已存在的标签保持不动。"""
    ApiError = upload_module().ApiError
    path = "/git/refs/tags/" + quote(NIGHTLY_TAG, safe="")
    try:
        if api.request("GET", path)["object"]["sha"]:
            return
    except ApiError as error:
        if error.status != 404:
            raise
    wait_for_commit(api, commit)
    try:
        api.request("POST", "/git/refs", {"ref": "refs/tags/" + NIGHTLY_TAG, "sha": commit})
    except ApiError as error:
        # 并发运行时另一个作业已创建标签，复用即可。
        if error.status != 422:
            raise


def find_nightly_release(api, tag):
    ApiError = upload_module().ApiError
    try:
        release = api.request("GET", "/releases/tags/" + quote(tag, safe=""))
    except ApiError as error:
        if error.status != 404:
            raise
        release = None
    if release is not None:
        if release["tag_name"] != tag:
            raise ValueError("Nightly release tag mismatch")
        return release
    # by-tag 接口可能遗漏草稿；列出来以便中断的运行复用同一发布。
    page = 1
    while True:
        batch = api.request("GET", f"/releases?per_page=100&page={page}")
        matching = [item for item in batch if item["tag_name"] == tag]
        if len(matching) > 1:
            raise ValueError("Multiple GitHub releases for the nightly tag")
        if matching:
            return matching[0]
        if len(batch) < 100:
            return None
        page += 1


def publish(api, commit, assets, body):
    ensure_github_tag(api, commit)
    release = find_nightly_release(api, NIGHTLY_TAG)
    if release is None:
        release = api.request("POST", "/releases", {
            "tag_name": NIGHTLY_TAG, "name": NIGHTLY_NAME, "prerelease": True, "draft": False,
            "target_commitish": commit, "body": body,
        })
    release_id = int(release["id"])
    existing = {asset["name"]: asset for asset in
                api.request("GET", f"/releases/{release_id}/assets?per_page=100")}
    for name in stale_assets(existing, assets):
        api.request("DELETE", f"/releases/assets/{existing[name]['id']}")
        del existing[name]
    for name, data in assets.items():
        digest = "sha256:" + hashlib.sha256(data).hexdigest()
        previous = existing.get(name)
        if previous is not None:
            # 同一提交重试时字节相同，直接复用已上传的资产。
            if previous.get("digest") == digest and previous.get("size") == len(data):
                continue
            api.request("DELETE", f"/releases/assets/{previous['id']}")
        uploaded = api.request(
            "POST", f"/releases/{release_id}/assets?name={quote(name, safe='')}", data, upload=True)
        if uploaded.get("digest") not in (None, digest):
            raise ValueError("Uploaded asset digest mismatch: " + name)
    updates = {"prerelease": True, "body": body}
    if release.get("draft"):
        updates["draft"] = False
    api.request("PATCH", f"/releases/{release_id}", updates)
    print(f"Nightly release now carries {commit}")


def gitea_call(method, url, data=None):
    token = os.environ.get("GITEA_TOKEN", "")
    if not token:
        raise ValueError("GITEA_TOKEN is required to keep the nightly tag")
    headers = {"Authorization": "token " + token, "Content-Type": "application/json"}
    request = Request(url, data=None if data is None else json.dumps(data).encode(),
                      headers=headers, method=method)
    try:
        with urlopen(request, timeout=60) as response:
            return json.loads(response.read() or b"{}")
    except HTTPError as error:
        raise ValueError(f"Gitea API returned HTTP {error.code} for {method} {url}") from None
    except URLError as error:
        raise ValueError(f"Gitea API unreachable: {error.reason}") from None


def tag_gitea(commit):
    """在 Gitea 中确保 nightly 标签存在，供 push mirror 同步到 GitHub。"""
    api = os.environ.get("GITEA_API_URL") or os.environ["GITHUB_SERVER_URL"].rstrip("/") + "/api/v1"
    base = f"{api}/repos/{os.environ['GITHUB_REPOSITORY']}/tags"
    for existing in gitea_call("GET", base + "?limit=50"):
        if existing["name"] == NIGHTLY_TAG:
            print(f"Nightly tag already exists at {existing['commit']['sha']}")
            return
    gitea_call("POST", base, {"tag_name": NIGHTLY_TAG, "target": commit})
    print(f"Created nightly tag at {commit}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("tag", "publish"))
    parser.add_argument("commit")
    parser.add_argument("--directory", type=Path, default=ROOT / "release-assets")
    parser.add_argument("--built-at", default="")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        parser.error("Expected a full source commit SHA")
    if args.command == "tag":
        tag_gitea(args.commit)
        return
    assets = expected_assets(args.directory)
    token = os.environ.get("RELEASE_UPLOAD", "")
    if not token:
        raise ValueError("RELEASE_UPLOAD is required to publish the nightly release")
    body = release_body(args.commit, os.environ.get("GITHUB_SERVER_URL", ""),
                        args.built_at or time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime()))
    publish(upload_module().GitHub(token), args.commit, assets, body)


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        sys.exit(str(error))