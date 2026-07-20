#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http3-benchmark-runtime"
h2load="${H2LOAD_BIN:-$project_root/temp/http3-bench-tools/build/nghttp2-bssl/src/h2load}"
backend="${BACKEND_BIN:-$project_root/build-bench-h3-off/example/http_benchmark_backend}"
lite="${LITE_GSO_OFF_BIN:-$project_root/build-bench-h3-off/apps/lite_nginx}"
rounds="${ROUNDS:-20}"
duration="${DURATION:-10}"
implementations="${IMPLEMENTATIONS:-lite-steal-on lite-steal-off}"
run_id="${RUN_ID:-churn-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/http3-benchmark-results/$run_id}"
sut_cpus="${SUT_CPUS:-0,2}"
sut_quota="${SUT_QUOTA:-none}"
backend_cpus="${BACKEND_CPUS:-14,16}"
backend_quota="${BACKEND_QUOTA:-none}"
client_cpus="${CLIENT_CPUS:-6,8,10,12}"
load_threads="${LOAD_THREADS:-4}"
h3_clients="${H3_CLIENTS:-8}"
h3_streams="${H3_STREAMS:-16}"
if (( load_threads > h3_clients )); then
    load_threads="$h3_clients"
fi
backend_unit="bench-h3-churn-backend-$run_id"
active_unit=""
status=0

stop_unit() {
    [[ -n "$1" ]] && systemctl --user stop "$1" >/dev/null 2>&1 || true
}

cleanup() {
    stop_unit "$active_unit"
    stop_unit "$backend_unit"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

wait_backend() {
    for attempt in $(seq 1 100); do
        curl -sf -o /dev/null http://127.0.0.1:19001/bench/1k && return 0
        sleep 0.1
    done
    return 1
}

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

mkdir -p "$result_dir/runs"
"$script_dir/prepare_runtime.sh" >/dev/null
printf 'implementation,round,load_status,active_after,health_status,result,exec_status,memory_peak,udp_in_errors,udp_rcvbuf_errors,udp_sndbuf_errors\n' \
    >"$result_dir/churn.csv"

{
    printf 'run_id=%s\n' "$run_id"
    printf 'git_commit=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
    printf 'implementations=%q\n' "$implementations"
    printf 'rounds=%s\n' "$rounds"
    printf 'duration_seconds=%s\n' "$duration"
    printf 'sut_cpus=%s\n' "$sut_cpus"
    printf 'backend_cpus=%s\n' "$backend_cpus"
    printf 'client_cpus=%s\n' "$client_cpus"
    printf 'load_threads=%s\n' "$load_threads"
    printf 'clients=%s\n' "$h3_clients"
    printf 'streams_per_client=%s\n' "$h3_streams"
} >"$result_dir/run.env"
git -C "$project_root" status --short >"$result_dir/git-status.txt"
sha256sum "$lite" "$backend" "$h2load" >"$result_dir/binary-sha256.txt"

stop_unit "$backend_unit"
systemctl --user reset-failed "$backend_unit" >/dev/null 2>&1 || true
backend_quota_property=()
[[ "$backend_quota" == none ]] || backend_quota_property=(--property="CPUQuota=$backend_quota")
systemd-run --user --unit="$backend_unit" \
    --property="CPUAffinity=$backend_cpus" "${backend_quota_property[@]}" \
    --property=CPUAccounting=yes --property=MemoryAccounting=yes \
    --working-directory="$project_root" "$backend" >/dev/null
wait_backend || exit 1

for implementation in $implementations; do
    case "$implementation" in
        lite-steal-on) config="$script_dir/configs/lite_nginx_steal_on.conf" ;;
        lite-steal-off) config="$script_dir/configs/lite_nginx_steal_off.conf" ;;
        lite-auto) config="$script_dir/configs/lite_nginx_auto.conf" ;;
        lite) config="$script_dir/configs/lite_nginx_steal_off.conf" ;;
        *) echo "unknown implementation: $implementation" >&2; exit 2 ;;
    esac
    for round in $(seq 1 "$rounds"); do
        unit="bench-h3-churn-$run_id-$implementation-$round"
        active_unit="$unit"
        output="$result_dir/runs/$implementation-$round"
        mkdir -p "$output"
        stop_unit "$unit"
        systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
        wait_clear || { echo "port did not clear" >&2; exit 1; }
        quota_property=()
        [[ "$sut_quota" == none ]] || quota_property=(--property="CPUQuota=$sut_quota")
        systemd-run --user --unit="$unit" \
            --property="CPUAffinity=$sut_cpus" "${quota_property[@]}" \
            --property=CPUAccounting=yes --property=MemoryAccounting=yes \
            --property=MemoryMax=1G --working-directory="$project_root" \
            "$lite" --config "$config" >/dev/null
        ready=0
        for attempt in $(seq 1 100); do
            if curl -ksf -o /dev/null https://127.0.0.1:18443/bench/1k; then
                ready=1
                break
            fi
            sleep 0.1
        done
        load_status=1
        nstat -az >"$output/nstat-before.txt" 2>&1 || true
        if [[ "$ready" == 1 ]]; then
            taskset -c "$client_cpus" "$h2load" --h3 --no-udp-gso \
                -t "$load_threads" -c "$h3_clients" -m "$h3_streams" -D "${duration}s" -N 10s \
                https://127.0.0.1:18443/bench/1k \
                https://127.0.0.1:18443/bench/64k \
                >"$output/h2load.out" 2>"$output/h2load.err"
            load_status=$?
        fi
        nstat -az >"$output/nstat-after.txt" 2>&1 || true
        active_after="$(systemctl --user show "$unit" -p ActiveState --value 2>/dev/null || true)"
        health_status=1
        if taskset -c 6 "$h2load" --h3 --no-udp-gso -t1 -c1 -m1 -n1 \
            https://127.0.0.1:18443/bench/1k >"$output/health.out" 2>"$output/health.err" && \
            rg -q '1 succeeded, 0 failed, 0 errored, 0 timeout' "$output/health.out"; then
            health_status=0
        fi
        result="$(systemctl --user show "$unit" -p Result --value 2>/dev/null || true)"
        exec_status="$(systemctl --user show "$unit" -p ExecMainStatus --value 2>/dev/null || true)"
        memory_peak="$(systemctl --user show "$unit" -p MemoryPeak --value 2>/dev/null || true)"
        udp_in_errors="$(counter_delta "$output/nstat-before.txt" "$output/nstat-after.txt" UdpInErrors)"
        udp_rcvbuf_errors="$(counter_delta "$output/nstat-before.txt" "$output/nstat-after.txt" UdpRcvbufErrors)"
        udp_sndbuf_errors="$(counter_delta "$output/nstat-before.txt" "$output/nstat-after.txt" UdpSndbufErrors)"
        journalctl --user -u "$unit" --no-pager >"$output/journal.log" 2>&1 || true
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$implementation" "$round" \
            "$load_status" "$active_after" "$health_status" "$result" "$exec_status" "$memory_peak" \
            "$udp_in_errors" "$udp_rcvbuf_errors" "$udp_sndbuf_errors" \
            >>"$result_dir/churn.csv"
        if [[ "$load_status" -ne 0 || "$active_after" != active || "$health_status" -ne 0 || \
              "$udp_in_errors" -ne 0 || "$udp_rcvbuf_errors" -ne 0 || "$udp_sndbuf_errors" -ne 0 ]]; then
            status=1
        fi
        stop_unit "$unit"
        active_unit=""
        systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    done
done

journalctl --user -u "$backend_unit" --no-pager >"$result_dir/backend-journal.log" 2>&1 || true
stop_unit "$backend_unit"
trap - EXIT INT TERM
exit "$status"
