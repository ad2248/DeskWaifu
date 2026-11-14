report_app_failure() {
    local exit_code=$1 log_file=$2
    echo "DeskWaifu exited with status $exit_code after quit" >&2
    if [[ -f $log_file ]]; then
        cat "$log_file" >&2 || true
    else
        echo "DeskWaifu log unavailable: $log_file" >&2
    fi
    return "$exit_code"
}
