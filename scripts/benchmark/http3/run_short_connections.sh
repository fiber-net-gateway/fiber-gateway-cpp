#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
h2load="${H2LOAD_BIN:-$project_root/temp/http3-bench-tools/build/nghttp2-bssl/src/h2load}"
direct_default="${DIRECT_GSO_OFF_BIN:-$project_root/build-bench-h3-off/example/http3_benchmark_server}"
direct_timerfd="${DIRECT_TIMERFD_BIN:-$project_root/build-bench-h3-timerfd/example/http3_benchmark_server}"
implementations="${IMPLEMENTATIONS:-direct direct-timerfd}"
connections="${CONNECTIONS:-100}"
run_id="${RUN_ID:-short-connections-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/http3-benchmark-results/$run_id}"
sut_cpus="${SUT_CPUS:-0,2}"
client_cpu="${CLIENT_CPU:-6}"
active_unit=""
status=0

stop_unit() {
    [[ -n "$1" ]] && systemctl --user stop "$1" >/dev/null 2>&1 || true
}

cleanup() {
    stop_unit "$active_unit"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

wait_clear() {
    for attempt in $(seq 1 100); do
        if ! ss -H -ltn | rg -q ':(18443)[[:space:]]' && \
           ! ss -H -lun | rg -q ':(18443)[[:space:]]'; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

counter_delta() {
    local before="$1"
    local after="$2"
    local key="$3"
    local before_value after_value
    before_value="$(awk -v key="$key" '$1 == key { print $2; exit }' "$before")"
    after_value="$(awk -v key="$key" '$1 == key { print $2; exit }' "$after")"
    if [[ "$before_value" =~ ^[0-9]+$ && "$after_value" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "$((after_value - before_value))"
    else
        printf '%s\n' -1
    fi
}

for executable in "$h2load" "$direct_default" "$direct_timerfd"; do
    [[ -x "$executable" ]] || { echo "missing executable: $executable" >&2; exit 1; }
done

"$script_dir/prepare_runtime.sh" >/dev/null
mkdir -p "$result_dir/runs"
printf 'implementation,round,status,succeeded,active_after\n' >"$result_dir/connections.csv"
printf 'implementation,udp_in_errors,udp_rcvbuf_errors,udp_sndbuf_errors\n' >"$result_dir/udp.csv"
{
    printf 'run_id=%s\n' "$run_id"
    printf 'git_commit=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
    printf 'implementations=%q\n' "$implementations"
    printf 'connections=%s\n' "$connections"
    printf 'sut_cpus=%s\n' "$sut_cpus"
    printf 'client_cpu=%s\n' "$client_cpu"
} >"$result_dir/run.env"
git -C "$project_root" status --short >"$result_dir/git-status.txt"
sha256sum "$direct_default" "$direct_timerfd" "$h2load" >"$result_dir/binary-sha256.txt"

for implementation in $implementations; do
    case "$implementation" in
        direct) executable="$direct_default" ;;
        direct-timerfd) executable="$direct_timerfd" ;;
        *) echo "unknown implementation: $implementation" >&2; exit 2 ;;
    esac
    unit="bench-h3-short-$run_id-$implementation"
    active_unit="$unit"
    stop_unit "$unit"
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    wait_clear || exit 1
    systemd-run --user --unit="$unit" \
        --property="CPUAffinity=$sut_cpus" \
        --property=CPUAccounting=yes --property=MemoryAccounting=yes \
        --working-directory="$project_root" \
        "$executable" 18443 2 "$project_root/build/http3-demo/cert.pem" \
        "$project_root/build/http3-demo/key.pem" >/dev/null

    ready=0
    for attempt in $(seq 1 100); do
        if curl -ksf -o /dev/null https://127.0.0.1:18443/bench/1k; then
            ready=1
            break
        fi
        sleep 0.1
    done
    [[ "$ready" == 1 ]] || { status=1; stop_unit "$unit"; active_unit=""; continue; }

    nstat -az >"$result_dir/runs/$implementation-nstat-before.txt" 2>&1 || true
    for round in $(seq 1 "$connections"); do
        output="$result_dir/runs/$implementation-$round.out"
        command_status=0
        taskset -c "$client_cpu" "$h2load" --h3 --no-udp-gso \
            --header-table-size=0 --encoder-header-table-size=0 \
            -t1 -c1 -m1 -n1 https://127.0.0.1:18443/bench/1k \
            >"$output" 2>"$output.err" || command_status=$?
        succeeded=0
        rg -q '1 succeeded, 0 failed, 0 errored, 0 timeout' "$output" && succeeded=1
        active="$(systemctl --user show "$unit" -p ActiveState --value 2>/dev/null || true)"
        printf '%s,%s,%s,%s,%s\n' "$implementation" "$round" "$command_status" "$succeeded" "$active" \
            >>"$result_dir/connections.csv"
        if [[ "$command_status" -ne 0 || "$succeeded" -ne 1 || "$active" != active ]]; then
            status=1
        fi
    done
    nstat -az >"$result_dir/runs/$implementation-nstat-after.txt" 2>&1 || true
    before="$result_dir/runs/$implementation-nstat-before.txt"
    after="$result_dir/runs/$implementation-nstat-after.txt"
    udp_in_errors="$(counter_delta "$before" "$after" UdpInErrors)"
    udp_rcvbuf_errors="$(counter_delta "$before" "$after" UdpRcvbufErrors)"
    udp_sndbuf_errors="$(counter_delta "$before" "$after" UdpSndbufErrors)"
    printf '%s,%s,%s,%s\n' "$implementation" "$udp_in_errors" "$udp_rcvbuf_errors" "$udp_sndbuf_errors" \
        >>"$result_dir/udp.csv"
    if [[ "$udp_in_errors" -ne 0 || "$udp_rcvbuf_errors" -ne 0 || "$udp_sndbuf_errors" -ne 0 ]]; then
        status=1
    fi
    journalctl --user -u "$unit" --no-pager >"$result_dir/runs/$implementation-journal.log" 2>&1 || true
    stop_unit "$unit"
    active_unit=""
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
done

trap - EXIT INT TERM
exit "$status"
