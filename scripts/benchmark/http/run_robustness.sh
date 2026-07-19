#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
result_dir="${RESULT_DIR:-$project_root/temp/http-benchmark-results/robustness}"
runtime_dir="$project_root/temp/http-benchmark-runtime"
tool_root="$project_root/temp/http-bench-tools/root"
h2load="$tool_root/usr/bin/h2load"
h2load_lib="$tool_root/usr/lib/x86_64-linux-gnu"
backend="$project_root/build-bench/example/http_benchmark_backend"
lite="$project_root/build-bench/apps/lite_nginx"
backend_unit="bench-http-robust-backend"
sut_unit="bench-http-robust-lite"
status=0

stop_units() {
    systemctl --user stop "$sut_unit" "$backend_unit" >/dev/null 2>&1 || true
}
trap stop_units EXIT

mkdir -p "$result_dir"
"$script_dir/prepare_runtime.sh" >/dev/null
stop_units
systemctl --user reset-failed "$sut_unit" "$backend_unit" >/dev/null 2>&1 || true

systemd-run --user --unit="$backend_unit" \
    --property=AllowedCPUs=14,16 --property=CPUQuota=200% \
    --property=CPUAccounting=yes --property=MemoryAccounting=yes \
    --working-directory="$project_root" "$backend" >/dev/null
for attempt in $(seq 1 100); do
    curl -sf -o /dev/null http://127.0.0.1:19001/bench/1k && break
    sleep 0.1
done

systemd-run --user --unit="$sut_unit" \
    --property=AllowedCPUs=0,2,4 --property=CPUQuota=200% \
    --property=CPUAccounting=yes --property=MemoryAccounting=yes \
    --property=MemoryMax=512M --working-directory="$project_root" \
    "$lite" --config "$script_dir/configs/lite_nginx_steal_off.conf" >/dev/null
for attempt in $(seq 1 100); do
    curl -sf -o /dev/null http://127.0.0.1:18080/bench/1k && break
    sleep 0.1
done

"$script_dir/robustness_http1.py" "$result_dir/http1-cases.csv" || status=1

run_fault() {
    local name="$1"
    local duration="$2"
    local path="$3"
    env LD_LIBRARY_PATH="$h2load_lib" taskset -c 6,8 "$h2load" \
        --alpn-list=h2 -t2 -c4 -m16 -D "${duration}s" -N 15s \
        --log-file="$result_dir/$name.tsv" \
        "https://127.0.0.1:18443$path" \
        >"$result_dir/$name.out" 2>"$result_dir/$name.err" || true
    "$script_dir/verify_response.sh" h2 >"$result_dir/$name-health.log" 2>&1 || status=1
}

run_fault upstream-delay 5 /fault/delay
run_fault upstream-close 3 /fault/close
run_fault upstream-partial 3 /fault/partial
run_fault upstream-hang 3 /fault/hang

systemctl --user stop "$backend_unit"
curl --insecure --silent --output "$result_dir/upstream-down.body" \
    --write-out '%{http_code}\n' https://127.0.0.1:18443/bench/1k \
    >"$result_dir/upstream-down.status" || true
systemd-run --user --unit="$backend_unit" \
    --property=AllowedCPUs=14,16 --property=CPUQuota=200% \
    --property=CPUAccounting=yes --property=MemoryAccounting=yes \
    --working-directory="$project_root" "$backend" >/dev/null
for attempt in $(seq 1 100); do
    curl -sf -o /dev/null http://127.0.0.1:19001/bench/1k && break
    sleep 0.1
done
recovery_start="$(date +%s%N)"
recovered=0
for attempt in $(seq 1 600); do
    if "$script_dir/verify_response.sh" h2 \
        >"$result_dir/upstream-recovery.log" 2>&1; then
        recovered=1
        break
    fi
    sleep 0.1
done
recovery_finish="$(date +%s%N)"
awk -v start="$recovery_start" -v finish="$recovery_finish" \
    -v attempts="$attempt" -v recovered="$recovered" \
    'BEGIN { printf "recovered=%d\nattempts=%d\nelapsed_seconds=%.3f\n", recovered, attempts, (finish - start) / 1000000000 }' \
    >"$result_dir/upstream-recovery.env"
[[ "$recovered" == 1 ]] || status=1

systemctl --user show "$sut_unit" \
    -p ActiveState -p SubState -p Result -p CPUUsageNSec -p MemoryPeak \
    >"$result_dir/sut-final.unit"
journalctl --user -u "$sut_unit" --no-pager >"$result_dir/sut-journal.log" 2>&1 || true
journalctl --user -u "$backend_unit" --no-pager >"$result_dir/backend-journal.log" 2>&1 || true

[[ "$(systemctl --user show "$sut_unit" -p ActiveState --value)" == active ]] || status=1
stop_units
trap - EXIT
exit "$status"
