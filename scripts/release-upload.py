#!/usr/bin/env python3
"""Publish verified artifacts to GitHub without replacing different existing assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from urllib.error import HTTPError
from urllib.parse import quote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

REPOSITORY = "ad2248/DeskWaifu"
API = f"https://api.github.com/repos/{REPOSITORY}"
UPLOADS = f"https://uploads.github.com/repos/{REPOSITORY}"


class SafeRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        if urlsplit(new_url).scheme != "https":
            raise ValueError("Refusing non-HTTPS GitHub redirect")
        redirected = super().redirect_request(request, fp, code, message, headers, new_url)
        if redirected is not None and urlsplit(new_url).netloc != urlsplit(request.full_url).netloc:
            # Asset downloads redirect to a signed CDN URL. urllib otherwise
            # forwards Authorization, unlike curl's default redirect policy.
            redirected.remove_header("Authorization")
        return redirected


urlopen = build_opener(SafeRedirect()).open


class ApiError(RuntimeError):
    def __init__(self, status, path):
        self.status = status
        super().__init__(f"GitHub API returned HTTP {status} for {path}")


class GitHub:
    def __init__(self, token):
        self.token = token

    def request(self, method, path, data=None, binary=False, upload=False):
        headers = {"Authorization": f"Bearer {self.token}", "X-GitHub-Api-Version": "2022-11-28",
                   "Accept": "application/octet-stream" if binary else "application/vnd.github+json",
                   "User-Agent": "DeskWaifu-release"}
        if isinstance(data, bytes):
            headers["Content-Type"] = "application/octet-stream"
        elif data is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(data).encode()
        # Never send the token to a server named by an API response or CLI input.
        request = Request((UPLOADS if upload else API) + path, data=data, headers=headers, method=method)
        try:
            with urlopen(request, timeout=120) as response:
                body = response.read()
        except HTTPError as error:
            raise ApiError(error.code, path) from None
        return body if binary else json.loads(body)


def optional_get(api, path):
    try:
        return api.request("GET", path)
    except ApiError as error:
        if error.status != 404:
            raise
        return None


def find_release(api, tag):
    release = optional_get(api, "/releases/tags/" + quote(tag, safe=""))
    if release is not None:
        return release
    # The by-tag endpoint may omit drafts. List with the authenticated token so
    # an interrupted draft is reused instead of repeatedly failing creation.
    page = 1
    while True:
        batch = api.request("GET", f"/releases?per_page=100&page={page}")
        matching = [release for release in batch if release["tag_name"] == tag]
        if len(matching) > 1:
            raise ValueError("Multiple GitHub releases for the same tag")
        if matching:
            return matching[0]
        if len(batch) < 100:
            return None
        page += 1


def tag_commit(api, tag):
    ref = optional_get(api, "/git/ref/tags/" + quote(tag, safe=""))
    if ref is None:
        return None
    obj = ref["object"]
    for _ in range(10):
        if obj["type"] == "commit":
            return obj["sha"]
        if obj["type"] != "tag":
            raise ValueError("GitHub release tag does not resolve to a commit")
        obj = api.request("GET", "/git/tags/" + obj["sha"])["object"]
    raise ValueError("Too many nested annotated tags")


def ensure_tag(api, tag, commit):
    existing = tag_commit(api, tag)
    if existing is None:
        # GitHub must already have the exact source commit (e.g. from mirroring).
        # A missing commit is NOT replaced with the GitHub default branch.
        actual = api.request("GET", "/git/commits/" + commit)
        if actual["sha"] != commit:
            raise ValueError("GitHub does not have the release source commit")
        try:
            api.request("POST", "/git/refs", {"ref": "refs/tags/" + tag, "sha": commit})
        except ApiError as error:
            if error.status != 422:
                raise
            # Another run may have just created the ref. Verify, never force it.
    if tag_commit(api, tag) != commit:
        raise ValueError("GitHub tag points to a different commit; refusing to release")


def list_assets(api, release_id):
    result = {}
    page = 1
    while True:
        batch = api.request("GET", f"/releases/{release_id}/assets?per_page=100&page={page}")
        for asset in batch:
            if asset["name"] in result:
                raise ValueError("Duplicate remote release asset names")
            result[asset["name"]] = asset
        if len(batch) < 100:
            return result
        page += 1


def check_asset(api, asset, data):
    digest = "sha256:" + hashlib.sha256(data).hexdigest()
    if asset.get("state") != "uploaded" or asset.get("size") != len(data):
        raise ValueError(f"Existing asset differs or is incomplete: {asset['name']}")
    remote_digest = asset.get("digest")
    if not remote_digest:
        remote = api.request("GET", f"/releases/assets/{asset['id']}", binary=True)
        remote_digest = "sha256:" + hashlib.sha256(remote).hexdigest()
    if remote_digest != digest:
        raise ValueError(f"Existing asset differs: {asset['name']}; never overwritten automatically")


def load_assets(directory, tag, commit):
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9]+(?:\.[A-Za-z0-9]+)*)?", tag):
        raise ValueError("Invalid release tag")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected a full source commit SHA")
    version = tag[1:].replace("-", "~", 1)
    names = {f"{name}-{version}-1-x86_64.pkg.tar.zst" for name in ("deskwaifu-bin", "deskwaifu-ocr-bin")}
    names.update(("release-metadata.json", "SHA256SUMS"))
    if {path.name for path in directory.iterdir()} != names:
        raise ValueError("Unexpected or missing release artifacts")
    assets = {name: (directory / name).read_bytes() for name in sorted(names)}
    manifest = json.loads(assets["release-metadata.json"])
    if manifest != {"tag": tag, "commit": commit, "pkgver": version, "pkgrel": 1, "ocr_models_included": False}:
        raise ValueError("Artifact manifest does not match selected source tag/commit")
    expected_sums = "".join(f"{hashlib.sha256(assets[name]).hexdigest()}  {name}\n"
                            for name in sorted(names - {"SHA256SUMS"}))
    if assets["SHA256SUMS"].decode() != expected_sums:
        raise ValueError("Release artifact checksum mismatch")
    return assets


def publish(api, tag, commit, assets):
    ensure_tag(api, tag, commit)
    release = find_release(api, tag)
    if release is None:
        try:
            release = api.request("POST", "/releases", {
                "tag_name": tag, "target_commitish": commit, "name": tag, "draft": True,
                "prerelease": "-" in tag,
                "body": f"Source commit: `{commit}`\n\nArch x86_64 binary packages. OCR models and dictionaries are not included.",
            })
        except ApiError as error:
            if error.status != 422:
                raise
            release = find_release(api, tag)
            if release is None:
                raise
    if release["tag_name"] != tag:
        raise ValueError("Release tag mismatch")
    target = release.get("target_commitish", "")
    if re.fullmatch(r"[0-9a-f]{40}", target) and target != commit:
        raise ValueError("Release target_commitish disagrees with source commit")
    release_id = int(release["id"])
    existing = list_assets(api, release_id)
    # Preflight every existing name before making any upload writes.
    for name, data in assets.items():
        if name in existing:
            check_asset(api, existing[name], data)
    for name, data in assets.items():
        if name in existing:
            print(f"Identical asset already present: {name}")
            continue
        try:
            uploaded = api.request("POST", f"/releases/{release_id}/assets?name={quote(name, safe='')}", data, upload=True)
        except ApiError as error:
            if error.status != 422:
                raise
            # Concurrent retry is safe only if the winning upload is identical.
            raced = list_assets(api, release_id).get(name)
            if raced is None:
                raise
            check_asset(api, raced, data)
        else:
            check_asset(api, uploaded, data)
    # Recheck tag immediately before publishing: never knowingly publish a moved tag.
    if tag_commit(api, tag) != commit:
        raise ValueError("GitHub tag moved during upload; release left unpublished")
    if release.get("draft"):
        api.request("PATCH", f"/releases/{release_id}", {"draft": False})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("tag")
    parser.add_argument("commit")
    args = parser.parse_args()
    assets = load_assets(args.directory, args.tag, args.commit)
    token = os.environ.get("RELEASE_UPLOAD", "")
    if not token:
        raise ValueError("RELEASE_UPLOAD is required only for the final upload step")
    publish(GitHub(token), args.tag, args.commit, assets)


if __name__ == "__main__":
    try:
        main()
    except (ApiError, ValueError) as error:
        sys.exit(str(error))
