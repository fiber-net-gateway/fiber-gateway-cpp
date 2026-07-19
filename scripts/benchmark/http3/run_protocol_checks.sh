#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http3-benchmark-runtime"
h2load="$project_root/temp/http3-bench-tools/build/nghttp2-bssl/src/h2load"
client="$project_root/temp/http3-bench-tools/build/ngtcp2-examples-libcxx/examples/bsslclient"
backend="$project_root/build-bench-h3-off/example/http_benchmark_backend"
lite="$project_root/build-bench-h3-off/apps/lite_nginx"
nginx="$project_root/temp/nginx-install/sbin/nginx"
curl_h3=/snap/bin/curl
implementations="${IMPLEMENTATIONS:-lite nginx}"
run_id="${RUN_ID:-protocol-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/http3-benchmark-results/$run_id}"
sut_cpus="${SUT_CPUS:-0}"
sut_quota="${SUT_QUOTA:-none}"
backend_unit="bench-h3-proto-backend-$run_id"
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

wait_clear() {
    for attempt in $(seq 1 100); do
        if ! ss -H -ltn | rg -q ':(18443)[[:space:]]' && \
           ! ss -H -lun | rg -q ':(18443)[[:space:]]'; then return 0; fi
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

health() {
    taskset -c 6 "$h2load" --h3 --no-udp-gso -t1 -c1 -m1 -n1 \
        https://127.0.0.1:18443/bench/1k >"$1" 2>&1 && \
        rg -q '1 succeeded, 0 failed, 0 errored, 0 timeout' "$1"
}

download() {
    local output_dir="$1"
    shift
    taskset -c 6 "$client" -q --no-gso --download="$output_dir" \
        --exit-on-all-streams-close "$@"
}

mkdir -p "$result_dir"
"$script_dir/prepare_runtime.sh" >/dev/null
printf 'implementation,case,passed,detail\n' >"$result_dir/checks.csv"

stop_unit "$backend_unit"
systemctl --user reset-failed "$backend_unit" >/dev/null 2>&1 || true
systemd-run --user --unit="$backend_unit" \
    --property=CPUAffinity=14,16 --property=CPUQuota=200% \
    --property=CPUAccounting=yes --property=MemoryAccounting=yes \
    --working-directory="$project_root" "$backend" >/dev/null
wait_backend || exit 1

for implementation in $implementations; do
    output="$result_dir/$implementation"
    body_dir="$output/body"
    mkdir -p "$body_dir"
    unit="bench-h3-proto-$run_id-$implementation"
    active_unit="$unit"
    wait_clear || exit 1
    case "$implementation" in
        lite)
            command=("$lite" --config "$script_dir/configs/lite_nginx_steal_off.conf")
            ;;
        nginx)
            command=("$nginx" -p "$project_root/" -c scripts/benchmark/http3/configs/nginx_gso_off.conf -g 'daemon off;')
            ;;
        *) echo "unknown implementation: $implementation" >&2; exit 2 ;;
    esac
    quota_property=()
    [[ "$sut_quota" == none ]] || quota_property=(--property="CPUQuota=$sut_quota")
    systemd-run --user --unit="$unit" \
        --property="CPUAffinity=$sut_cpus" "${quota_property[@]}" \
        --property=CPUAccounting=yes --property=MemoryAccounting=yes \
        --property=MemoryMax=1G --working-directory="$project_root" \
        "${command[@]}" >/dev/null
    for attempt in $(seq 1 100); do
        curl -ksf -o /dev/null https://127.0.0.1:18443/bench/1k && break
        sleep 0.1
    done

    passed=0
    if download "$body_dir" 127.0.0.1 18443 https://127.0.0.1:18443/bench/1k && \
       [[ "$(sha256sum "$body_dir/1k" | cut -d' ' -f1)" == 2edc986847e209b4016e141a6dc8716d3207350f416969382d431539bf292e4a ]]; then passed=1; fi
    printf '%s,get-1k,%s,sha256\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    passed=0
    if taskset -c 6 "$curl_h3" --http3-only --insecure --fail --silent --show-error \
       --output "$body_dir/curl-1k" https://127.0.0.1:18443/bench/1k && \
       [[ "$(sha256sum "$body_dir/curl-1k" | cut -d' ' -f1)" == 2edc986847e209b4016e141a6dc8716d3207350f416969382d431539bf292e4a ]]; then passed=1; fi
    printf '%s,curl-http3-get-1k,%s,second-client\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    passed=0
    if download "$body_dir" 127.0.0.1 18443 https://127.0.0.1:18443/bench/64k && \
       [[ "$(sha256sum "$body_dir/64k" | cut -d' ' -f1)" == a0a24a08a87ed054cd2e20aa994bcd25e5266f8c5435011ac4982987f4e3a370 ]]; then passed=1; fi
    printf '%s,get-64k,%s,sha256\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    passed=0
    if download "$body_dir" -m POST -d "$runtime_dir/request_64k.bin" \
       127.0.0.1 18443 https://127.0.0.1:18443/bench/echo && \
       [[ "$(sha256sum "$body_dir/echo" | cut -d' ' -f1)" == de2f256064a0af797747c2b97505dc0b9f3df0de4f489eac731c23ae9ca9cc31 ]]; then passed=1; fi
    printf '%s,post-echo-64k,%s,sha256\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    key_dir="$output/key-update"
    mkdir -p "$key_dir"
    passed=0
    if taskset -c 6 "$client" --no-gso --key-update=100ms \
       --qlog-file="$output/key-update.qlog" --download="$key_dir" \
       --exit-on-all-streams-close 127.0.0.1 18443 \
       https://127.0.0.1:18443/fault/delay >"$output/key-update.out" 2>"$output/key-update.err" && \
       [[ "$(sha256sum "$key_dir/delay" | cut -d' ' -f1)" == 2edc986847e209b4016e141a6dc8716d3207350f416969382d431539bf292e4a ]] && \
       [[ "$(rg -c 'Initiate key update' "$output/key-update.err")" -ge 2 ]]; then passed=1; fi
    printf '%s,key-update-twice,%s,peer-initiated\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    migration_dir="$output/migration"
    mkdir -p "$migration_dir"
    passed=0
    if taskset -c 6 "$client" -q --no-gso --change-local-addr=20ms --nat-rebinding \
       --download="$migration_dir" --exit-on-all-streams-close 127.0.0.1 18443 \
       https://127.0.0.1:18443/fault/delay >"$output/migration.out" 2>"$output/migration.err" && \
       [[ "$(sha256sum "$migration_dir/delay" | cut -d' ' -f1)" == 2edc986847e209b4016e141a6dc8716d3207350f416969382d431539bf292e4a ]]; then passed=1; fi
    printf '%s,nat-rebinding,%s,path-change\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    passed=0
    if taskset -c 6 "$client" -q --no-gso -t 0.01 -r 0.01 -n 10 \
       --exit-on-all-streams-close 127.0.0.1 18443 \
       https://127.0.0.1:18443/bench/64k >"$output/loss.out" 2>"$output/loss.err"; then passed=1; fi
    printf '%s,loss-1pct,%s,client-tx-rx\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    python3 - 127.0.0.1 18443 <<'PY'
import os
import socket
import sys

host, port = sys.argv[1], int(sys.argv[2])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for size in (0, 1, 7, 31, 255, 1199):
    for _ in range(20):
        sock.sendto(os.urandom(size), (host, port))
PY
    passed=0
    health "$output/post-datagram-health.out" && passed=1
    printf '%s,bounded-invalid-datagrams,%s,120-packets\n' "$implementation" "$passed" >>"$result_dir/checks.csv"
    [[ "$passed" == 1 ]] || status=1

    journalctl --user -u "$unit" --no-pager >"$output/journal.log" 2>&1 || true
    systemctl --user show "$unit" -p ActiveState -p Result -p ExecMainStatus -p MemoryPeak >"$output/unit.txt"
    stop_unit "$unit"
    active_unit=""
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
done

stop_unit "$backend_unit"
trap - EXIT INT TERM
exit "$status"
