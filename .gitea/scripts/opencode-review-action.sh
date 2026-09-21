#!/usr/bin/env bash
set -euo pipefail

repo="${GITHUB_REPOSITORY:-kals/DeskWaifu}"
repo_root="${REVIEW_REPO_ROOT:?missing REVIEW_REPO_ROOT}"
pr_number="${REVIEW_PR_NUMBER:?missing REVIEW_PR_NUMBER}"
tested_sha="${REVIEW_HEAD_SHA:?missing REVIEW_HEAD_SHA}"
marker="${REVIEW_MARKER:?missing REVIEW_MARKER}"

current_head() {
  tea api "/repos/${repo}/pulls/${pr_number}" \
    --login reviewer --repo "$repo_root" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["head"]["sha"])'
}

require_current_head() {
  local head
  head="$(current_head)"
  test "$head" = "$tested_sha" || {
    echo "PR head changed: $head != $tested_sha" >&2
    exit 4
  }
}

action="${1:-}"
case "$action" in
  head)
    require_current_head
    printf '%s\n' "$tested_sha"
    ;;
  issues)
    tea api "/repos/${repo}/issues?state=open&type=issues" \
      --login reviewer --repo "$repo_root"
    ;;
  create-issue)
    test "$#" -eq 3 || { echo "usage: $0 create-issue TITLE BODY" >&2; exit 2; }
    require_current_head
    tea issues create --title "$2" --description "$3" \
      --login reviewer --repo "$repo_root"
    ;;
  approve)
    test "$#" -eq 2 || { echo "usage: $0 approve BODY" >&2; exit 2; }
    require_current_head
    printf -v body '%s\n%s' "$marker" "$2"
    tea api "/repos/${repo}/pulls/${pr_number}/reviews" \
      --login reviewer --repo "$repo_root" -X POST \
      -f body="$body" -f commit_id="$tested_sha" -f event=APPROVE >/dev/null
    printf 'approved %s\n' "$tested_sha"
    ;;
  *)
    echo "unsupported review action: $action" >&2
    exit 2
    ;;
esac
