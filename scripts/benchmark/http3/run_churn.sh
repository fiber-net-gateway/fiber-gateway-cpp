#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http3-benchmark-runtime"
h2load="$project_root/temp/http3-bench-tools/build/nghttp2-bssl/src/h2load"
backend="$project_root/build-bench-h3-off/example/http_benchmark_backend"
lite="$project_root/build-bench-h3-off/apps/lite_nginx"
rounds="${ROUNDS:-20}"
duration="${DURATION:-10}"
implementations="${IMPLEMENTATIONS:-lite-auto lite}"
run_id="${RUN_ID:-churn-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/http3-benchmark-results/$run_id}"
sut_cpus="${SUT_CPUS:-0}"
sut_quota="${SUT_QUOTA:-none}"
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

mkdir -p "$result_dir/runs"
"$script_dir/prepare_runtime.sh" >/dev/null
printf 'implementation,round,load_status,active_after,health_status,result,exec_status,memory_peak\n' \
    >"$result_dir/churn.csv"

stop_unit "$backend_unit"
systemctl --user reset-failed "$backend_unit" >/dev/null 2>&1 || true
systemd-run --user --unit="$backend_unit" \
    --property=CPUAffinity=14,16 --property=CPUQuota=200% \
    --property=CPUAccounting=yes --property=MemoryAccounting=yes \
    --working-directory="$project_root" "$backend" >/dev/null
wait_backend || exit 1

for implementation in $implementations; do
    case "$implementation" in
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
        if [[ "$ready" == 1 ]]; then
            taskset -c 6,8,10,12 "$h2load" --h3 --no-udp-gso \
                -t4 -c8 -m64 -D "${duration}s" -N 10s \
                https://127.0.0.1:18443/bench/1k \
                https://127.0.0.1:18443/bench/64k \
                >"$output/h2load.out" 2>"$output/h2load.err"
            load_status=$?
        fi
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
        journalctl --user -u "$unit" --no-pager >"$output/journal.log" 2>&1 || true
        printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$implementation" "$round" \
            "$load_status" "$active_after" "$health_status" "$result" "$exec_status" "$memory_peak" \
            >>"$result_dir/churn.csv"
        if [[ "$load_status" -ne 0 || "$active_after" != active || "$health_status" -ne 0 ]]; then
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
