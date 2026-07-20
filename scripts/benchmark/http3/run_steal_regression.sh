#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
h2load="${H2LOAD_BIN:-$project_root/temp/http3-bench-tools/build/nghttp2-bssl/src/h2load}"
backend="${BACKEND_BIN:-$project_root/build-bench-h3-off/example/http_benchmark_backend}"
lite="${LITE_TRACE_BIN:-${LITE_GSO_OFF_BIN:-$project_root/build-bench-h3-off/apps/lite_nginx}}"
implementations="${IMPLEMENTATIONS:-lite-steal-on lite-steal-off}"
delayed_cancel_rounds="${DELAYED_CANCEL_ROUNDS:-${CANCEL_ROUNDS:-8}}"
blocked_cancel_rounds="${BLOCKED_CANCEL_ROUNDS:-${CANCEL_ROUNDS:-100}}"
cancel_after="${CANCEL_AFTER:-0.15}"
run_id="${RUN_ID:-steal-regression-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/http3-benchmark-results/$run_id}"
sut_cpus="${SUT_CPUS:-0,2}"
backend_cpus="${BACKEND_CPUS:-14,16}"
client_cpus="${CLIENT_CPUS:-6,8,10,12}"
backend_unit="bench-h3-steal-backend-$run_id"
active_unit=""
status=0

unit_property() {
    systemctl --user show "$1" --property="$2" --value 2>/dev/null || true
}

stop_unit() {
    local unit="$1"
    [[ -n "$unit" ]] || return
    systemctl --user stop "$unit" >/dev/null 2>&1 || true
    for attempt in $(seq 1 100); do
        local state
        state="$(unit_property "$unit" ActiveState)"
        [[ "$state" != active && "$state" != deactivating ]] && return
        sleep 0.1
    done
    systemctl --user kill --signal=SIGKILL --kill-whom=all "$unit" >/dev/null 2>&1 || true
}

cleanup() {
    stop_unit "$active_unit"
    stop_unit "$backend_unit"
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

wait_backend() {
    for attempt in $(seq 1 100); do
        curl -sf -o /dev/null http://127.0.0.1:19001/bench/1k && return 0
        sleep 0.1
    done
    return 1
}

start_backend() {
    stop_unit "$backend_unit"
    systemctl --user reset-failed "$backend_unit" >/dev/null 2>&1 || true
    systemd-run --user --unit="$backend_unit" \
        --property="CPUAffinity=$backend_cpus" \
        --property=CPUAccounting=yes --property=MemoryAccounting=yes \
        --working-directory="$project_root" "$backend" >/dev/null && wait_backend
}

h3_health() {
    local output="$1"
    taskset -c "$client_cpus" "$h2load" --h3 --no-udp-gso \
        --header-table-size=0 --encoder-header-table-size=0 \
        -t1 -c1 -m1 -n1 https://127.0.0.1:18443/bench/1k \
        >"$output" 2>"$output.err" && \
        rg -q '1 succeeded, 0 failed, 0 errored, 0 timeout' "$output"
}

record_case() {
    local implementation="$1"
    local case_name="$2"
    local round="$3"
    local command_status="$4"
    local output_dir="$5"
    local health_status=1
    h3_health "$output_dir/health.out" && health_status=0
    local active
    active="$(unit_property "$active_unit" ActiveState)"
    printf '%s,%s,%s,%s,%s,%s\n' "$implementation" "$case_name" "$round" \
        "$command_status" "$health_status" "$active" >>"$result_dir/results.csv"
    if [[ "$health_status" -ne 0 || "$active" != active ]]; then
        status=1
    fi
}

for executable in "$h2load" "$backend" "$lite"; do
    [[ -x "$executable" ]] || { echo "missing executable: $executable" >&2; exit 1; }
done
"$script_dir/prepare_runtime.sh" >/dev/null
mkdir -p "$result_dir/runs"
printf 'implementation,case,round,command_status,health_status,active_after\n' >"$result_dir/results.csv"
{
    printf 'run_id=%s\n' "$run_id"
    printf 'git_commit=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
    printf 'implementations=%q\n' "$implementations"
    printf 'delayed_cancel_rounds=%s\n' "$delayed_cancel_rounds"
    printf 'blocked_cancel_rounds=%s\n' "$blocked_cancel_rounds"
    printf 'cancel_after_seconds=%s\n' "$cancel_after"
    printf 'sut_cpus=%s\n' "$sut_cpus"
    printf 'backend_cpus=%s\n' "$backend_cpus"
    printf 'client_cpus=%s\n' "$client_cpus"
} >"$result_dir/run.env"
git -C "$project_root" status --short >"$result_dir/git-status.txt"
sha256sum "$lite" "$backend" "$h2load" >"$result_dir/binary-sha256.txt"

start_backend || { echo "backend failed to start" >&2; exit 1; }

for implementation in $implementations; do
    case "$implementation" in
        lite-steal-on) config="$script_dir/configs/lite_nginx_steal_on.conf" ;;
        lite-steal-off) config="$script_dir/configs/lite_nginx_steal_off.conf" ;;
        *) echo "unknown implementation: $implementation" >&2; exit 2 ;;
    esac

    unit="bench-h3-steal-$run_id-$implementation"
    active_unit="$unit"
    stop_unit "$unit"
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    wait_clear || { echo "benchmark port did not clear" >&2; exit 1; }
    systemd-run --user --unit="$unit" \
        --property="CPUAffinity=$sut_cpus" \
        --property=CPUAccounting=yes --property=MemoryAccounting=yes --property=MemoryMax=1G \
        --working-directory="$project_root" \
        /usr/bin/env FIBER_HTTP_POOL_TRACE=1 "$lite" --config "$config" >/dev/null

    ready=0
    for attempt in $(seq 1 100); do
        if h3_health "$result_dir/runs/$implementation-ready.out"; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "SUT failed to become ready: $implementation" >&2
        status=1
        stop_unit "$unit"
        active_unit=""
        continue
    fi

    nstat -az >"$result_dir/runs/$implementation-nstat-before.txt" 2>&1 || true

    # Serial new QUIC connections move between reuseport shards and make an idle
    # upstream connection available for a deterministic remote-steal preflight.
    preflight_dir="$result_dir/runs/$implementation-steal-preflight"
    mkdir -p "$preflight_dir"
    preflight_status=0
    for round in $(seq 1 100); do
        h3_health "$preflight_dir/$round.out" || preflight_status=1
    done
    record_case "$implementation" steal-preflight 1 "$preflight_status" "$preflight_dir"

    for case_name in delayed-cancel blocked-cancel; do
        case "$case_name" in
            delayed-cancel)
                path=/fault/delay
                rounds="$delayed_cancel_rounds"
                clients=8
                streams=16
                requests=128
                ;;
            blocked-cancel)
                path=/fault/hang
                rounds="$blocked_cancel_rounds"
                clients=8
                streams=1
                requests=8
                ;;
        esac
        for round in $(seq 1 "$rounds"); do
            output_dir="$result_dir/runs/$implementation-$case_name-$round"
            mkdir -p "$output_dir"
            command_status=0
            timeout --signal=TERM --kill-after=1s "${cancel_after}s" \
                taskset -c "$client_cpus" "$h2load" --h3 --no-udp-gso \
                --header-table-size=0 --encoder-header-table-size=0 \
                -t4 -c "$clients" -m "$streams" -n "$requests" -N 10s \
                "https://127.0.0.1:18443$path" \
                >"$output_dir/h2load.out" 2>"$output_dir/h2load.err" || command_status=$?
            record_case "$implementation" "$case_name" "$round" "$command_status" "$output_dir"
        done
    done

    for case_name in upstream-close upstream-partial; do
        case "$case_name" in
            upstream-close) path=/fault/close ;;
            upstream-partial) path=/fault/partial ;;
        esac
        output_dir="$result_dir/runs/$implementation-$case_name"
        mkdir -p "$output_dir"
        command_status=0
        taskset -c "$client_cpus" "$h2load" --h3 --no-udp-gso \
            --header-table-size=0 --encoder-header-table-size=0 \
            -t2 -c4 -m8 -D 3s -N 10s "https://127.0.0.1:18443$path" \
            >"$output_dir/h2load.out" 2>"$output_dir/h2load.err" || command_status=$?
        record_case "$implementation" "$case_name" 1 "$command_status" "$output_dir"
    done

    output_dir="$result_dir/runs/$implementation-backend-restart"
    mkdir -p "$output_dir"
    taskset -c "$client_cpus" "$h2load" --h3 --no-udp-gso \
        --header-table-size=0 --encoder-header-table-size=0 \
        -t2 -c4 -m8 -D 5s -N 10s https://127.0.0.1:18443/bench/1k \
        >"$output_dir/h2load.out" 2>"$output_dir/h2load.err" &
    load_pid=$!
    sleep 1
    stop_unit "$backend_unit"
    sleep 0.2
    restart_status=0
    start_backend || restart_status=1
    wait "$load_pid" || true
    record_case "$implementation" backend-restart 1 "$restart_status" "$output_dir"

    nstat -az >"$result_dir/runs/$implementation-nstat-after.txt" 2>&1 || true
    stop_unit "$unit"
    active_unit=""
    journalctl --user --unit="$unit" --no-pager >"$result_dir/runs/$implementation-journal.log" 2>&1 || true
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true

    if [[ "$implementation" == lite-steal-on ]]; then
        trace_line="$(rg 'FIBER_HTTP_POOL_TRACE local_hit' \
            "$result_dir/runs/$implementation-journal.log" | head -n1 || true)"
        printf '%s\n' "$trace_line" >"$result_dir/steal-trace.txt"
        remote_hits="$(sed -n 's/.*remote_hit=\([0-9][0-9]*\).*/\1/p' <<<"$trace_line")"
        if [[ ! "$remote_hits" =~ ^[1-9][0-9]*$ ]]; then
            echo "steal trace did not record a remote hit" >&2
            status=1
        fi
    fi
done

journalctl --user --unit="$backend_unit" --no-pager >"$result_dir/backend-journal.log" 2>&1 || true
stop_unit "$backend_unit"
trap - EXIT INT TERM
exit "$status"
