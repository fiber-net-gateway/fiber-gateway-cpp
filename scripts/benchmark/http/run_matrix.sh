#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http-benchmark-runtime"
tool_root="$project_root/temp/http-bench-tools/root"
h2load="${H2LOAD_BIN:-$tool_root/usr/bin/h2load}"
h2load_library_path="$tool_root/usr/lib/x86_64-linux-gnu"
lite_nginx="${LITE_NGINX_BIN:-$project_root/build-bench/apps/lite_nginx}"
nginx="${NGINX_BIN:-$project_root/temp/nginx-install/sbin/nginx}"
backend="${BACKEND_BIN:-$project_root/build-bench/example/http_benchmark_backend}"
lite_config="${LITE_CONFIG:-$script_dir/configs/lite_nginx_steal_off.conf}"
nginx_config="${NGINX_CONFIG:-$script_dir/configs/nginx_sut.conf}"
plain_port="${PLAIN_PORT:-18080}"
tls_port="${TLS_PORT:-18443}"
backend_port="${BACKEND_PORT:-19001}"

repetitions="${REPETITIONS:-7}"
duration="${DURATION:-60}"
warmup="${WARMUP:-15}"
cooldown="${COOLDOWN:-30}"
implementations="${IMPLEMENTATIONS:-lite nginx}"
case_filter="${CASE_FILTER:-}"
run_id="${RUN_ID:-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/http-benchmark-results/$run_id}"

backend_cpus="${BACKEND_CPUS:-14,16}"
sut_cpus="${SUT_CPUS:-0,2,4}"
client_cpus="${CLIENT_CPUS:-6,8,10,12}"
load_threads="${LOAD_THREADS:-4}"
backend_quota="${BACKEND_QUOTA:-200%}"
sut_quota="${SUT_QUOTA:-200%}"

backend_unit="bench-http-backend-$run_id"
active_sut_unit=""
overall_status=0

usage() {
    cat <<'EOF'
Usage: run_matrix.sh

Environment overrides:
  REPETITIONS=7 DURATION=60 WARMUP=15 COOLDOWN=30
  IMPLEMENTATIONS="lite nginx" CASE_FILTER="H2-T-1K"
  RUN_ID=<name> RESULT_DIR=<directory>

The default "lite" implementation uses steal off. Use IMPLEMENTATIONS="lite-auto"
for the separate connection-stealing robustness reproduction.
EOF
}

if [[ "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

log() {
    printf '[%s] %s\n' "$(date --iso-8601=seconds)" "$*"
}

unit_property() {
    local unit="$1"
    local property="$2"
    systemctl --user show "$unit" --property="$property" --value 2>/dev/null || true
}

snapshot_unit() {
    local unit="$1"
    local output="$2"
    systemctl --user show "$unit" \
        --property=ActiveState \
        --property=SubState \
        --property=Result \
        --property=ExecMainStatus \
        --property=ExecMainCode \
        --property=ControlGroup \
        --property=AllowedCPUs \
        --property=CPUQuotaPerSecUSec \
        --property=CPUUsageNSec \
        --property=MemoryCurrent \
        --property=MemoryPeak \
        --property=TasksCurrent \
        >"$output" 2>&1 || true
}

stop_unit() {
    local unit="$1"
    if [[ -z "$unit" ]]; then
        return
    fi
    systemctl --user stop "$unit" >/dev/null 2>&1 || true
    if [[ "$(unit_property "$unit" ActiveState)" == "active" ]]; then
        systemctl --user kill --signal=SIGKILL --kill-whom=all "$unit" \
            >/dev/null 2>&1 || true
    fi
    local attempt
    for attempt in $(seq 1 100); do
        if [[ "$(unit_property "$unit" ActiveState)" != "active" ]] && \
           [[ "$(unit_property "$unit" ActiveState)" != "deactivating" ]]; then
            break
        fi
        sleep 0.1
    done
}

cleanup() {
    stop_unit "$active_sut_unit"
    stop_unit "$backend_unit"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

require_file() {
    if [[ ! -x "$1" ]]; then
        echo "required executable is missing: $1" >&2
        exit 1
    fi
}

wait_for_backend() {
    local attempt
    for attempt in $(seq 1 100); do
        if curl --silent --fail --output /dev/null \
            "http://127.0.0.1:$backend_port/bench/1k"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

ports_are_clear() {
    ! ss -H -ltn | rg -q ":($plain_port|$tls_port)[[:space:]]"
}

wait_for_ports_clear() {
    local attempt
    for attempt in $(seq 1 100); do
        if ports_are_clear; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_sut() {
    local unit="$1"
    local attempt
    for attempt in $(seq 1 100); do
        if [[ "$(unit_property "$unit" ActiveState)" == "failed" ]] || \
           [[ "$(unit_property "$unit" ActiveState)" == "inactive" ]]; then
            return 1
        fi
        if [[ "$(unit_property "$unit" ActiveState)" == "active" ]] && \
           curl --silent --fail --output /dev/null \
            "http://127.0.0.1:$plain_port/bench/1k"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

start_backend() {
    local quota_property=()
    if [[ "$backend_quota" != none ]]; then
        quota_property=(--property="CPUQuota=$backend_quota")
    fi
    log "starting backend unit=$backend_unit cpus=$backend_cpus quota=$backend_quota"
    systemd-run --user --unit="$backend_unit" \
        --property="AllowedCPUs=$backend_cpus" \
        "${quota_property[@]}" \
        --property=CPUAccounting=yes \
        --property=MemoryAccounting=yes \
        --working-directory="$project_root" \
        "$backend" "$backend_port" >/dev/null
    if ! wait_for_backend; then
        journalctl --user --unit="$backend_unit" --no-pager -n 100 >&2
        exit 1
    fi
}

start_sut() {
    local implementation="$1"
    local unit="$2"
    local command=()
    local quota_property=()

    case "$implementation" in
        lite)
            command=("$lite_nginx" --config "$lite_config")
            ;;
        lite-auto)
            command=("$lite_nginx" --config "$script_dir/configs/lite_nginx.conf")
            ;;
        nginx|openresty)
            command=("$nginx" -p "$project_root/" -c "$nginx_config" -g 'daemon off;')
            ;;
        *)
            echo "unknown implementation: $implementation" >&2
            return 2
            ;;
    esac

    if ! wait_for_ports_clear; then
        echo "benchmark ports are still occupied before starting $implementation" >&2
        ss -ltnp | rg ":($plain_port|$tls_port)" >&2 || true
        return 1
    fi
    rm -f "$runtime_dir/nginx-sut.pid"
    if [[ "$sut_quota" != none ]]; then
        quota_property=(--property="CPUQuota=$sut_quota")
    fi
    log "starting implementation=$implementation unit=$unit cpus=$sut_cpus quota=$sut_quota"
    systemd-run --user --unit="$unit" \
        --property="AllowedCPUs=$sut_cpus" \
        "${quota_property[@]}" \
        --property=CPUAccounting=yes \
        --property=MemoryAccounting=yes \
        --working-directory="$project_root" \
        "${command[@]}" >/dev/null
    active_sut_unit="$unit"
    if ! wait_for_sut "$unit"; then
        journalctl --user --unit="$unit" --no-pager -n 100 >&2
        return 1
    fi
}

case_settings() {
    local case_name="$1"
    h2_protocol=()
    h2_data=()
    h2_clients=128
    h2_streams=1
    verify_mode=plain
    target_url="http://127.0.0.1:$plain_port/bench/1k"

    case "$case_name" in
        H1-P-1K)
            h2_protocol=(--h1)
            ;;
        H1-P-64K)
            h2_protocol=(--h1)
            target_url="http://127.0.0.1:$plain_port/bench/64k"
            ;;
        H1-P-1M)
            h2_protocol=(--h1)
            target_url="http://127.0.0.1:$plain_port/bench/1m"
            ;;
        H1-P-POST-1M)
            h2_protocol=(--h1)
            h2_data=(-d "$runtime_dir/request_1m.bin")
            target_url="http://127.0.0.1:$plain_port/bench/echo"
            ;;
        H1-T-1K)
            h2_protocol=(--h1)
            verify_mode=tls-h1
            target_url="https://127.0.0.1:$tls_port/bench/1k"
            ;;
        H1-T-64K)
            h2_protocol=(--h1)
            verify_mode=tls-h1
            target_url="https://127.0.0.1:$tls_port/bench/64k"
            ;;
        H2-T-1K)
            h2_protocol=(--alpn-list=h2)
            h2_clients=8
            h2_streams=64
            verify_mode=h2
            target_url="https://127.0.0.1:$tls_port/bench/1k"
            ;;
        H2-T-64K)
            h2_protocol=(--alpn-list=h2)
            h2_clients=8
            h2_streams=64
            verify_mode=h2
            target_url="https://127.0.0.1:$tls_port/bench/64k"
            ;;
        H2-T-1M)
            h2_protocol=(--alpn-list=h2)
            h2_clients=8
            h2_streams=64
            verify_mode=h2
            target_url="https://127.0.0.1:$tls_port/bench/1m"
            ;;
        H2-T-POST-1M)
            h2_protocol=(--alpn-list=h2)
            h2_data=(-d "$runtime_dir/request_1m.bin")
            h2_clients=8
            h2_streams=64
            verify_mode=h2
            target_url="https://127.0.0.1:$tls_port/bench/echo"
            ;;
        *)
            echo "unknown case: $case_name" >&2
            return 2
            ;;
    esac

    if [[ "$case_name" == H2-* ]]; then
        h2_clients="${H2_CLIENTS_OVERRIDE:-$h2_clients}"
        h2_streams="${H2_STREAMS_OVERRIDE:-$h2_streams}"
    fi
}

run_case() {
    local case_name="$1"
    local implementation="$2"
    local repetition="$3"
    local sequence="$4"
    local unit="bench-http-sut-$run_id-$sequence"
    local output_dir="$result_dir/runs/$case_name/rep-$repetition/$implementation"
    local load_pid load_status=0
    local warmup_options=()

    mkdir -p "$output_dir"
    case_settings "$case_name" || return
    if ! start_sut "$implementation" "$unit"; then
        echo 1 >"$output_dir/start.status"
        overall_status=1
        stop_unit "$unit"
        active_sut_unit=""
        return
    fi

    PLAIN_PORT="$plain_port" TLS_PORT="$tls_port" \
        "$script_dir/verify_response.sh" "$verify_mode" \
        >"$output_dir/verify.log" 2>&1
    if [[ $? -ne 0 ]]; then
        log "response verification failed: case=$case_name implementation=$implementation"
        overall_status=1
    fi

    {
        printf 'case=%s\n' "$case_name"
        printf 'implementation=%s\n' "$implementation"
        printf 'repetition=%s\n' "$repetition"
        printf 'duration_seconds=%s\n' "$duration"
        printf 'warmup_seconds=%s\n' "$warmup"
        printf 'clients=%s\n' "$h2_clients"
        printf 'streams_per_client=%s\n' "$h2_streams"
        printf 'load_threads=%s\n' "$load_threads"
        printf 'target_url=%s\n' "$target_url"
        printf 'sut_unit=%s\n' "$unit"
        printf 'backend_unit=%s\n' "$backend_unit"
        printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
    } >"$output_dir/meta.env"

    log "running case=$case_name repetition=$repetition implementation=$implementation"
    if [[ "$warmup" != "0" ]]; then
        warmup_options=(--warm-up-time="${warmup}s")
    fi
    /usr/bin/time -v -o "$output_dir/h2load.time" \
        env LD_LIBRARY_PATH="$h2load_library_path" \
        taskset -c "$client_cpus" "$h2load" \
        "${h2_protocol[@]}" \
        -t "$load_threads" -c "$h2_clients" -m "$h2_streams" \
        -D "${duration}s" "${warmup_options[@]}" \
        -N 10s --log-file="$output_dir/requests.tsv" \
        "${h2_data[@]}" "$target_url" \
        >"$output_dir/h2load.out" 2>"$output_dir/h2load.err" &
    load_pid=$!

    sleep "$warmup"
    snapshot_unit "$unit" "$output_dir/sut-before.unit"
    snapshot_unit "$backend_unit" "$output_dir/backend-before.unit"
    "$script_dir/collect_cgroup.sh" "$unit" "$output_dir/sut-before" \
        >/dev/null 2>&1 || true
    "$script_dir/collect_cgroup.sh" "$backend_unit" "$output_dir/backend-before" \
        >/dev/null 2>&1 || true

    wait "$load_pid" || load_status=$?
    echo "$load_status" >"$output_dir/h2load.status"
    snapshot_unit "$unit" "$output_dir/sut-after.unit"
    snapshot_unit "$backend_unit" "$output_dir/backend-after.unit"
    "$script_dir/collect_cgroup.sh" "$unit" "$output_dir/sut-after" \
        >/dev/null 2>&1 || true
    "$script_dir/collect_cgroup.sh" "$backend_unit" "$output_dir/backend-after" \
        >/dev/null 2>&1 || true
    ss -s >"$output_dir/socket-summary-after.txt"
    cat /proc/loadavg >"$output_dir/loadavg-after.txt"
    sleep 1
    snapshot_unit "$unit" "$output_dir/sut-settled.unit"
    journalctl --user --unit="$unit" --no-pager \
        >"$output_dir/sut-journal.log" 2>&1 || true

    if [[ "$load_status" -ne 0 ]]; then
        log "load generator failed status=$load_status case=$case_name implementation=$implementation"
        overall_status=1
    fi
    if [[ "$implementation" != "lite-auto" ]] && \
       [[ "$(unit_property "$unit" ActiveState)" != "active" ]]; then
        log "SUT exited unexpectedly: case=$case_name implementation=$implementation"
        overall_status=1
    fi

    stop_unit "$unit"
    active_sut_unit=""
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    if [[ "$cooldown" != "0" ]]; then
        sleep "$cooldown"
    fi
}

require_file "$h2load"
require_file "$lite_nginx"
require_file "$nginx"
require_file "$backend"
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
    printf 'client_cpus=%s\n' "$client_cpus"
    printf 'backend_quota=%s\n' "$backend_quota"
    printf 'sut_quota=%s\n' "$sut_quota"
    printf 'plain_port=%s\n' "$plain_port"
    printf 'tls_port=%s\n' "$tls_port"
    printf 'backend_port=%s\n' "$backend_port"
    printf 'lite_config=%s\n' "$lite_config"
    printf 'nginx_config=%s\n' "$nginx_config"
    printf 'git_commit=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
    printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
} >"$result_dir/run.env"

uname -a >"$result_dir/uname.txt"
lscpu >"$result_dir/lscpu.txt"
free -h >"$result_dir/memory.txt"
ulimit -a >"$result_dir/ulimit.txt"
sysctl kernel.hostname kernel.osrelease net.ipv4.ip_local_port_range \
    net.core.somaxconn net.ipv4.tcp_max_syn_backlog \
    >"$result_dir/sysctl.txt" 2>&1 || true
git -C "$project_root" status --short >"$result_dir/git-status.txt"
sha256sum "$lite_nginx" "$nginx" "$backend" >"$result_dir/binary-sha256.txt"
lite_build_dir="$(cd "$(dirname "$lite_nginx")/.." && pwd)"
cp "$lite_build_dir/CMakeCache.txt" "$result_dir/CMakeCache.txt"
env LD_LIBRARY_PATH="$h2load_library_path" "$h2load" --version \
    >"$result_dir/h2load-version.txt" 2>&1
"$nginx" -V >"$result_dir/nginx-version.txt" 2>&1
"$lite_nginx" --help >"$result_dir/lite-nginx-help.txt" 2>&1 || true

start_backend
snapshot_unit "$backend_unit" "$result_dir/backend-start.unit"

cases=(
    H1-P-1K
    H1-P-64K
    H1-P-1M
    H1-P-POST-1M
    H2-T-1K
    H2-T-64K
    H2-T-1M
    H2-T-POST-1M
)
sequence=0
for case_name in "${cases[@]}"; do
    if [[ -n "$case_filter" && "$case_name" != *"$case_filter"* ]]; then
        continue
    fi
    for repetition in $(seq 1 "$repetitions"); do
        read -r -a ordered_implementations <<<"$implementations"
        if (( repetition % 2 == 0 )); then
            reversed=()
            for (( index=${#ordered_implementations[@]} - 1; index >= 0; --index )); do
                reversed+=("${ordered_implementations[index]}")
            done
            ordered_implementations=("${reversed[@]}")
        fi
        for implementation in "${ordered_implementations[@]}"; do
            sequence=$((sequence + 1))
            run_case "$case_name" "$implementation" "$repetition" "$sequence"
        done
    done
done

snapshot_unit "$backend_unit" "$result_dir/backend-finish.unit"
journalctl --user --unit="$backend_unit" --no-pager \
    >"$result_dir/backend-journal.log" 2>&1 || true
stop_unit "$backend_unit"
systemctl --user reset-failed "$backend_unit" >/dev/null 2>&1 || true
trap - EXIT INT TERM

"$script_dir/summarize.py" "$result_dir"
log "results=$result_dir status=$overall_status"
exit "$overall_status"
