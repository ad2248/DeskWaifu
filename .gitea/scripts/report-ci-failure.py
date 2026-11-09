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


def sign(body: bytes, secret: bytes) -> str:
    return hmac.new(secret, body, hashlib.sha256).hexdigest()


def failed_jobs(env: dict) -> list:
    """列出本次失败涉及的作业。

    ci.yml 只由一个聚合作业上报（否则每个失败作业都会创建一次智能体运行），
    因此 GITHUB_JOB 恒为 report，必须靠 CI_FAILED_JOBS 传回真实失败作业名。
    """
    return [name for name in env.get("CI_FAILED_JOBS", "").split() if name]


def build_payload(event: dict, env: dict | None = None) -> dict | None:
    """按事件构造 ci_failed 载荷；非 PR 事件返回 None。

    自动化用 pull_request_number 选会话、用 logs 拼提示词，两者都不能缺：
    logs 为 null 时上游的 join() 会抛类型错误，事件根本到不了会话。
    """
    env = os.environ if env is None else env
    pull_request = event.get("pull_request") or {}
    number = pull_request.get("number") or 0
    if not number:
        return None

    repository = env["GITHUB_REPOSITORY"]
    server = env.get("GITHUB_SERVER_URL", "")
    run_id = env.get("GITHUB_RUN_ID", "")
    run_url = "{}/{}/actions/runs/{}".format(server, repository, run_id)
    job = ", ".join(failed_jobs(env)) or env.get("GITHUB_JOB", "")
    return {
        "action": "ci_failed",
        "repository": {"full_name": repository},
        "pull_request": {"number": number},
        "pull_request_number": number,
        "branch": (pull_request.get("head") or {}).get("ref", ""),
        "run_url": run_url,
        "workflow": env.get("GITHUB_WORKFLOW", "ci.yml"),
        "job": job,
        # 智能体需要能自行取回日志：给出运行页与作业列表 API，避免在载荷里塞整份日志
        "logs": "作业「{}」的日志见 {} ；可用 {} 列出作业后取 /actions/jobs/<id>/logs".format(
            job or "CI",
            run_url,
            "{}/api/v1/repos/{}/actions/runs/{}/jobs".format(server, repository, run_id),
        ),
        "status": "failure",
    }


def main() -> int:
    with open(os.environ["GITHUB_EVENT_PATH"]) as fp:
        event = json.load(fp)

    payload = build_payload(event)
    if payload is None:
        print("非 PR 事件，跳过回调")
        return 0

    secret = os.environ.get("PIPELINE_WEBHOOK_SECRET", "")
    if not secret:
        # 未配密钥时宁可显式失败，也不要发出无法验证的请求
        print("未配置 PIPELINE_WEBHOOK_SECRET，跳过回调（事件未上报）", file=sys.stderr)
        return 1

    body = json.dumps(payload, separators=(",", ":")).encode()
    request = urllib.request.Request(
        EVENTS_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "X-Gitea-Signature": sign(body, secret.encode()),
        },
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        print("回调返回", response.status, response.read().decode()[:200])
    return 0


if __name__ == "__main__":
    sys.exit(main())
