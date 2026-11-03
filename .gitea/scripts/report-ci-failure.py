"""CI 失败时把事件上报给 OpenHands 自动化，由智能体在同一 PR 会话中修复。

只处理 PR 触发的失败：push 到默认分支的失败没有 PR 号，上报会生成无意义的会话。
"""

import hashlib
import hmac
import json
import os
import sys
import urllib.request

EVENTS_URL = (
    "https://hand.kals.top/api/automation/v1/events/"
    "46d09c5c-7d7d-585f-8c37-a6b45a3de738/gitea"
)


def build_payload(event: dict) -> dict | None:
    """按事件构造 ci_failed 载荷；非 PR 事件返回 None。"""
    pull_request = event.get("pull_request") or {}
    number = pull_request.get("number") or 0
    if not number:
        return None
    return {
        "action": "ci_failed",
        "repository": {"full_name": os.environ["GITHUB_REPOSITORY"]},
        "pull_request": {"number": number},
        "branch": (pull_request.get("head") or {}).get("ref", ""),
        "run_url": "{}/{}/actions/runs/{}".format(
            os.environ.get("GITHUB_SERVER_URL", ""),
            os.environ["GITHUB_REPOSITORY"],
            os.environ.get("GITHUB_RUN_ID", ""),
        ),
        "workflow": os.environ.get("GITHUB_WORKFLOW", "ci.yml"),
        "status": "failure",
    }


def main() -> int:
    with open(os.environ["GITHUB_EVENT_PATH"]) as fp:
        event = json.load(fp)

    payload = build_payload(event)
    if payload is None:
        print("非 PR 事件，跳过回调")
        return 0

    body = json.dumps(payload, separators=(",", ":")).encode()
    secret = os.environ["PIPELINE_WEBHOOK_SECRET"].encode()
    signature = hmac.new(secret, body, hashlib.sha256).hexdigest()

    request = urllib.request.Request(
        EVENTS_URL,
        data=body,
        headers={"Content-Type": "application/json", "X-Gitea-Signature": signature},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        print("回调返回", response.status, response.read().decode()[:200])
    return 0


if __name__ == "__main__":
    sys.exit(main())
