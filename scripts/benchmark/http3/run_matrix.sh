#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http3-benchmark-runtime"
h2load="$project_root/temp/http3-bench-tools/build/nghttp2-bssl/src/h2load"
lite_off="$project_root/build-bench-h3-off/apps/lite_nginx"
lite_on="$project_root/build-bench-h3-on/apps/lite_nginx"
nginx="$project_root/temp/nginx-install/sbin/nginx"
backend="$project_root/build-bench-h3-off/example/http_benchmark_backend"
collect_cgroup="$script_dir/../http/collect_cgroup.sh"

repetitions="${REPETITIONS:-9}"
duration="${DURATION:-60}"
warmup="${WARMUP:-20}"
cooldown="${COOLDOWN:-30}"
implementations="${IMPLEMENTATIONS:-lite nginx}"
case_filter="${CASE_FILTER:-}"
run_id="${RUN_ID:-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/http3-benchmark-results/$run_id}"

backend_cpus="${BACKEND_CPUS:-14,16}"
sut_cpus="${SUT_CPUS:-0,2,4}"
client_cpus="${CLIENT_CPUS:-6,8,10,12}"
load_threads="${LOAD_THREADS:-4}"
backend_quota="${BACKEND_QUOTA:-200%}"
sut_quota="${SUT_QUOTA:-100%}"
h3_clients="${H3_CLIENTS:-8}"
h3_streams="${H3_STREAMS:-64}"

backend_unit="bench-h3-backend-$run_id"
active_sut_unit=""
overall_status=0

log() {
    printf '[%s] %s\n' "$(date --iso-8601=seconds)" "$*"
}

unit_property() {
    systemctl --user show "$1" --property="$2" --value 2>/dev/null || true
}

snapshot_unit() {
    local main_pid
    systemctl --user show "$1" \
        --property=ActiveState --property=SubState --property=Result \
        --property=ExecMainStatus --property=ExecMainCode \
        --property=ControlGroup --property=MainPID \
        --property=CPUQuotaPerSecUSec --property=CPUUsageNSec \
        --property=MemoryCurrent --property=MemoryPeak --property=TasksCurrent \
        >"$2" 2>&1 || true
    main_pid="$(unit_property "$1" MainPID)"
    if [[ "$main_pid" =~ ^[1-9][0-9]*$ ]]; then
        taskset -pc "$main_pid" >>"$2" 2>&1 || true
    fi
}

stop_unit() {
    local unit="$1"
    [[ -n "$unit" ]] || return
    systemctl --user stop "$unit" >/dev/null 2>&1 || true
    for attempt in $(seq 1 100); do
        local state
        state="$(unit_property "$unit" ActiveState)"
        if [[ "$state" != active && "$state" != deactivating ]]; then
            return
        fi
        sleep 0.1
    done
    systemctl --user kill --signal=SIGKILL --kill-whom=all "$unit" >/dev/null 2>&1 || true
}

cleanup() {
    stop_unit "$active_sut_unit"
    stop_unit "$backend_unit"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ports_are_clear() {
    ! ss -H -ltn | rg -q ':(18443)[[:space:]]' && \
        ! ss -H -lun | rg -q ':(18443)[[:space:]]'
}

wait_for_ports_clear() {
    for attempt in $(seq 1 100); do
        ports_are_clear && return 0
        sleep 0.1
    done
    return 1
}

wait_for_backend() {
    for attempt in $(seq 1 100); do
        curl --silent --fail --output /dev/null http://127.0.0.1:19001/bench/1k && return 0
        sleep 0.1
    done
    return 1
}

start_backend() {
    log "starting backend cpus=$backend_cpus quota=$backend_quota"
    systemd-run --user --unit="$backend_unit" \
        --property="CPUAffinity=$backend_cpus" --property="CPUQuota=$backend_quota" \
        --property=CPUAccounting=yes --property=MemoryAccounting=yes \
        --working-directory="$project_root" "$backend" >/dev/null
    wait_for_backend
}

start_sut() {
    local implementation="$1"
    local unit="$2"
    local command=()
    local quota_property=()
    case "$implementation" in
        lite)
            command=("$lite_off" --config "$script_dir/configs/lite_nginx_steal_off.conf")
            ;;
        lite-auto)
            command=("$lite_off" --config "$script_dir/configs/lite_nginx_auto.conf")
            ;;
        lite-gso)
            command=("$lite_on" --config "$script_dir/configs/lite_nginx_steal_off.conf")
            ;;
        nginx)
            command=("$nginx" -p "$project_root/" -c scripts/benchmark/http3/configs/nginx_gso_off.conf -g 'daemon off;')
            ;;
        nginx-gso)
            command=("$nginx" -p "$project_root/" -c scripts/benchmark/http3/configs/nginx_gso_on.conf -g 'daemon off;')
            ;;
        *)
            echo "unknown implementation: $implementation" >&2
            return 2
            ;;
    esac

    if [[ "$sut_quota" != none ]]; then
        quota_property=(--property="CPUQuota=$sut_quota")
    fi

    wait_for_ports_clear || return 1
    log "starting implementation=$implementation cpus=$sut_cpus quota=$sut_quota"
    systemd-run --user --unit="$unit" \
        --property="CPUAffinity=$sut_cpus" "${quota_property[@]}" \
        --property=CPUAccounting=yes --property=MemoryAccounting=yes \
        --working-directory="$project_root" "${command[@]}" >/dev/null
    active_sut_unit="$unit"
    for attempt in $(seq 1 100); do
        local state
        state="$(unit_property "$unit" ActiveState)"
        [[ "$state" == failed || "$state" == inactive ]] && return 1
        if curl --insecure --silent --fail --output /dev/null \
            https://127.0.0.1:18443/bench/1k && \
            ss -H -lun | rg -q ':(18443)[[:space:]]'; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

case_settings() {
    target_url=https://127.0.0.1:18443/bench/1k
    expected_status=200
    expected_bytes=1024
    data_options=()
    case "$1" in
        H3-GET-1K)
            ;;
        H3-GET-64K)
            target_url=https://127.0.0.1:18443/bench/64k
            expected_bytes=65536
            ;;
        H3-POST-64K)
            target_url=https://127.0.0.1:18443/bench/echo
            expected_bytes=65536
            data_options=(-d "$runtime_dir/request_64k.bin")
            ;;
        *)
            echo "unknown case: $1" >&2
            return 2
            ;;
    esac
}

run_case() {
    local case_name="$1"
    local implementation="$2"
    local repetition="$3"
    local sequence="$4"
    local unit="bench-h3-sut-$run_id-$sequence"
    local output_dir="$result_dir/runs/$case_name/rep-$repetition/$implementation"
    local load_status=0
    local gate_status=0
    local client_gso=off
    local gso_options=(--no-udp-gso)
    [[ "$implementation" == *-gso ]] && client_gso=on && gso_options=()

    mkdir -p "$output_dir"
    case_settings "$case_name" || return
    if ! start_sut "$implementation" "$unit"; then
        echo 1 >"$output_dir/start.status"
        journalctl --user --unit="$unit" --no-pager -n 100 >"$output_dir/sut-journal.log" 2>&1 || true
        overall_status=1
        stop_unit "$unit"
        active_sut_unit=""
        return
    fi

    taskset -c "$client_cpus" "$h2load" --h3 \
        --header-table-size=0 --encoder-header-table-size=0 "${gso_options[@]}" \
        -t1 -c1 -m1 -n1 "${data_options[@]}" "$target_url" \
        >"$output_dir/gate.out" 2>"$output_dir/gate.err" || gate_status=$?
    if [[ "$gate_status" -ne 0 ]] || \
       ! rg -q 'requests: 1 total, 1 started, 1 done, 1 succeeded, 0 failed' "$output_dir/gate.out" || \
       ! rg -q "status codes: 1 2xx" "$output_dir/gate.out" || \
       ! rg -q "\\(${expected_bytes}\\) data|${expected_bytes} data" "$output_dir/gate.out"; then
        log "HTTP/3 gate failed case=$case_name implementation=$implementation"
        gate_status=1
        overall_status=1
    fi
    echo "$gate_status" >"$output_dir/gate.status"

    {
        printf 'case=%s\n' "$case_name"
        printf 'implementation=%s\n' "$implementation"
        printf 'repetition=%s\n' "$repetition"
        printf 'duration_seconds=%s\n' "$duration"
        printf 'warmup_seconds=%s\n' "$warmup"
        printf 'clients=%s\n' "$h3_clients"
        printf 'streams_per_client=%s\n' "$h3_streams"
        printf 'load_threads=%s\n' "$load_threads"
        printf 'client_gso=%s\n' "$client_gso"
        printf 'target_url=%s\n' "$target_url"
        printf 'expected_status=%s\n' "$expected_status"
        printf 'expected_bytes=%s\n' "$expected_bytes"
        printf 'sut_unit=%s\n' "$unit"
        printf 'backend_unit=%s\n' "$backend_unit"
        printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
    } >"$output_dir/meta.env"

    log "running case=$case_name repetition=$repetition implementation=$implementation"
    nstat -az >"$output_dir/nstat-before.txt" 2>&1 || true
    cp /proc/net/snmp "$output_dir/snmp-before.txt" 2>/dev/null || true
    /usr/bin/time -v -o "$output_dir/h2load.time" \
        taskset -c "$client_cpus" "$h2load" --h3 \
        --header-table-size=0 --encoder-header-table-size=0 "${gso_options[@]}" \
        -t "$load_threads" -c "$h3_clients" -m "$h3_streams" \
        -D "${duration}s" --warm-up-time="${warmup}s" -N 15s \
        --log-file="$output_dir/requests.tsv" \
        --output-file="$output_dir/h2load.json" \
        "${data_options[@]}" "$target_url" \
        >"$output_dir/h2load.out" 2>"$output_dir/h2load.err" &
    local load_pid=$!

    sleep "$warmup"
    snapshot_unit "$unit" "$output_dir/sut-before.systemd-unit"
    snapshot_unit "$backend_unit" "$output_dir/backend-before.systemd-unit"
    "$collect_cgroup" "$unit" "$output_dir/sut-before" >/dev/null 2>&1 || true
    "$collect_cgroup" "$backend_unit" "$output_dir/backend-before" >/dev/null 2>&1 || true

    wait "$load_pid" || load_status=$?
    echo "$load_status" >"$output_dir/h2load.status"
    snapshot_unit "$unit" "$output_dir/sut-after.systemd-unit"
    snapshot_unit "$backend_unit" "$output_dir/backend-after.systemd-unit"
    "$collect_cgroup" "$unit" "$output_dir/sut-after" >/dev/null 2>&1 || true
    "$collect_cgroup" "$backend_unit" "$output_dir/backend-after" >/dev/null 2>&1 || true
    ss -s >"$output_dir/socket-summary-after.txt"
    nstat -az >"$output_dir/nstat-after.txt" 2>&1 || true
    cp /proc/net/snmp "$output_dir/snmp-after.txt" 2>/dev/null || true
    journalctl --user --unit="$unit" --no-pager >"$output_dir/sut-journal.log" 2>&1 || true

    if [[ "$load_status" -ne 0 ]] || [[ "$(unit_property "$unit" ActiveState)" != active ]]; then
        overall_status=1
    fi
    stop_unit "$unit"
    active_sut_unit=""
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    [[ "$cooldown" == 0 ]] || sleep "$cooldown"
}

for executable in "$h2load" "$lite_off" "$lite_on" "$nginx" "$backend"; do
    [[ -x "$executable" ]] || { echo "missing executable: $executable" >&2; exit 1; }
done
"$script_dir/prepare_runtime.sh" >/dev/null
mkdir -p "$result_dir/runs"

{
    printf 'run_id=%s\n' "$run_id"
    printf 'repetitions=%s\n' "$repetitions"
    printf 'duration_seconds=%s\n' "$duration"
    printf 'warmup_seconds=%s\n' "$warmup"
    printf 'cooldown_seconds=%s\n' "$cooldown"
    printf 'implementations=%q\n' "$implementations"
    printf 'backend_cpus=%s\n' "$backend_cpus"
    printf 'sut_cpus=%s\n' "$sut_cpus"
    printf 'cpu_binding=systemd CPUAffinity\n'
    printf 'client_cpus=%s\n' "$client_cpus"
    printf 'backend_quota=%s\n' "$backend_quota"
    printf 'sut_quota=%s\n' "$sut_quota"
    printf 'git_commit=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
    printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
} >"$result_dir/run.env"
uname -a >"$result_dir/uname.txt"
lscpu >"$result_dir/lscpu.txt"
free -h >"$result_dir/memory.txt"
sysctl net.core.rmem_max net.core.wmem_max net.core.netdev_max_backlog \
    net.ipv4.ip_local_port_range >"$result_dir/sysctl.txt" 2>&1 || true
git -C "$project_root" status --short >"$result_dir/git-status.txt"
sha256sum "$lite_off" "$lite_on" "$nginx" "$backend" "$h2load" >"$result_dir/binary-sha256.txt"
cp "$project_root/build-bench-h3-off/CMakeCache.txt" "$result_dir/CMakeCache-lite-off.txt"
cp "$project_root/build-bench-h3-on/CMakeCache.txt" "$result_dir/CMakeCache-lite-on.txt"
"$h2load" --version >"$result_dir/h2load-version.txt"
"$nginx" -V >"$result_dir/nginx-version.txt" 2>&1

start_backend || { echo "backend failed to start" >&2; exit 1; }
snapshot_unit "$backend_unit" "$result_dir/backend-start.unit"

cases=(H3-GET-1K H3-GET-64K H3-POST-64K)
sequence=0
for case_name in "${cases[@]}"; do
    [[ -z "$case_filter" || "$case_name" == *"$case_filter"* ]] || continue
    for repetition in $(seq 1 "$repetitions"); do
        read -r -a ordered <<<"$implementations"
        if (( repetition % 2 == 0 )); then
            reversed=()
            for (( index=${#ordered[@]} - 1; index >= 0; --index )); do
                reversed+=("${ordered[index]}")
            done
            ordered=("${reversed[@]}")
        fi
        for implementation in "${ordered[@]}"; do
            sequence=$((sequence + 1))
            run_case "$case_name" "$implementation" "$repetition" "$sequence"
        done
    done
done

snapshot_unit "$backend_unit" "$result_dir/backend-finish.unit"
journalctl --user --unit="$backend_unit" --no-pager >"$result_dir/backend-journal.log" 2>&1 || true
stop_unit "$backend_unit"
systemctl --user reset-failed "$backend_unit" >/dev/null 2>&1 || true
trap - EXIT INT TERM

"$script_dir/summarize.py" "$result_dir"
log "results=$result_dir status=$overall_status"
exit "$overall_status"
