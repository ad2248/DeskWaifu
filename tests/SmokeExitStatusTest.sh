#!/usr/bin/env bash
set -euo pipefail
source "$1/scripts/quit-exit-status.sh"
set +e
report_app_failure 42 /nonexistent/deskwaifu.log >/dev/null 2>&1
result=$?
set -e
test "$result" -eq 42
