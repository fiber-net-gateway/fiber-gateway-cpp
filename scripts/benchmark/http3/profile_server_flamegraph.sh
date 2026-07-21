#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"

server_bin="${SERVER_BIN:-$project_root/build-profile-http3-jemalloc/example/http3_benchmark_server}"
client_bin="${CLIENT_BIN:-$project_root/build-release/example/http3_benchmark_client}"
cert_file="${CERT_FILE:-$project_root/build/http3-demo/cert.pem}"
key_file="${KEY_FILE:-$project_root/build/http3-demo/key.pem}"
stackcollapse="${STACKCOLLAPSE:-$project_root/temp/FlameGraph/stackcollapse-perf.pl}"
flamegraph="${FLAMEGRAPH:-$project_root/temp/FlameGraph/flamegraph.pl}"

port="${PORT:-18443}"
server_workers="${SERVER_WORKERS:-2}"
sut_cpus="${SUT_CPUS:-0,1}"
client_cpus="${CLIENT_CPUS:-2,3}"
client_threads="${CLIENT_THREADS:-2}"
client_connections="${CLIENT_CONNECTIONS:-2}"
warmup_seconds="${WARMUP_SECONDS:-2}"
duration_seconds="${DURATION_SECONDS:-20}"
drain_seconds="${DRAIN_SECONDS:-5}"
perf_frequency="${PERF_FREQUENCY:-499}"
perf_event="${PERF_EVENT:-cycles:u}"
cases_text="${CASES:-get-1k get-64k get-1m post-64k}"
run_id="${RUN_ID:-http3-server-profile-$(date +%Y%m%dT%H%M%S)}"
result_parent="$project_root/temp/http3-flamegraphs"
result_dir="$result_parent/$run_id"

active_server_pid=""
active_perf_pid=""
overall_status=0

usage() {
    cat <<'EOF'
Collect on-CPU flame graphs from the jemalloc HTTP/3 benchmark server with
server and client QUIC pacing disabled.

Run from the repository root:
  sudo scripts/benchmark/http3/profile_server_flamegraph.sh

Useful overrides:
  sudo env CASES="get-64k get-1m" DURATION_SECONDS=30 \
    scripts/benchmark/http3/profile_server_flamegraph.sh

Supported CASES values:
  get-1k get-64k get-1m post-64k

Other overrides:
  RUN_ID, PORT, SERVER_WORKERS, SUT_CPUS, CLIENT_CPUS, CLIENT_THREADS,
  CLIENT_CONNECTIONS, WARMUP_SECONDS, DURATION_SECONDS, DRAIN_SECONDS,
  PERF_FREQUENCY, PERF_EVENT, SERVER_BIN, CLIENT_BIN, CERT_FILE, KEY_FILE,
  STACKCOLLAPSE, FLAMEGRAPH, MALLOC_CONF.
EOF
}

fail() {
    echo "error: $*" >&2
    exit 1
}

log() {
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

require_positive_integer() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || fail "$name must be a positive integer: $value"
}

stop_pid() {
    local pid="$1"
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 0
    kill -0 "$pid" 2>/dev/null || {
        wait "$pid" 2>/dev/null || true
        return 0
    }

    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
        local state=""
        [[ -r "/proc/$pid/stat" ]] && state="$(awk '{ print $3 }' "/proc/$pid/stat" 2>/dev/null || true)"
        if ! kill -0 "$pid" 2>/dev/null || [[ "$state" == Z ]]; then
            wait "$pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

restore_result_owner() {
    local owner_uid="${SUDO_UID:-0}"
    local owner_gid="${SUDO_GID:-0}"
    if [[ "$owner_uid" =~ ^[0-9]+$ && "$owner_gid" =~ ^[0-9]+$ && "$owner_uid" -ne 0 ]]; then
        chown -R "$owner_uid:$owner_gid" "$result_dir" 2>/dev/null || true
    fi
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "$active_perf_pid" ]]; then
        kill -INT "$active_perf_pid" 2>/dev/null || true
        wait "$active_perf_pid" 2>/dev/null || true
        active_perf_pid=""
    fi
    if [[ -n "$active_server_pid" ]]; then
        stop_pid "$active_server_pid"
        active_server_pid=""
    fi
    restore_result_owner
    exit "$status"
}

port_is_listening() {
    ss -H -ltnu | awk -v suffix=":$port" '$5 ~ (suffix "$") { found = 1 } END { exit !found }'
}

udp_port_is_listening() {
    ss -H -lun | awk -v suffix=":$port" '$4 ~ (suffix "$") { found = 1 } END { exit !found }'
}

wait_for_server() {
    for _ in $(seq 1 100); do
        if ! kill -0 "$active_server_pid" 2>/dev/null; then
            return 1
        fi
        if udp_port_is_listening && curl --http1.1 --insecure --silent --fail \
            --max-time 1 --output /dev/null "https://127.0.0.1:$port/bench/1k"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_port_clear() {
    for _ in $(seq 1 50); do
        port_is_listening || return 0
        sleep 0.1
    done
    return 1
}

write_run_metadata() {
    {
        printf 'run_id=%s\n' "$run_id"
        printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
        printf 'project_root=%s\n' "$project_root"
        printf 'git_commit=%s\n' "$(git -c safe.directory="$project_root" -C "$project_root" rev-parse HEAD)"
        printf 'server_bin=%s\n' "$server_bin"
        printf 'client_bin=%s\n' "$client_bin"
        printf 'server_allocator=jemalloc\n'
        printf 'server_pacing=off\n'
        printf 'client_pacing=off\n'
        printf 'port=%s\n' "$port"
        printf 'server_workers=%s\n' "$server_workers"
        printf 'sut_cpus=%s\n' "$sut_cpus"
        printf 'client_cpus=%s\n' "$client_cpus"
        printf 'client_threads=%s\n' "$client_threads"
        printf 'client_connections=%s\n' "$client_connections"
        printf 'warmup_seconds=%s\n' "$warmup_seconds"
        printf 'duration_seconds=%s\n' "$duration_seconds"
        printf 'drain_seconds=%s\n' "$drain_seconds"
        printf 'perf_frequency=%s\n' "$perf_frequency"
        printf 'perf_event=%s\n' "$perf_event"
        printf 'cases=%q\n' "$cases_text"
        printf 'malloc_conf=%q\n' "${MALLOC_CONF:-}"
    } >"$result_dir/run.env"

    git -c safe.directory="$project_root" -C "$project_root" status --short >"$result_dir/git-status.txt"
    sha256sum "$server_bin" "$client_bin" >"$result_dir/binary-sha256.txt"
    uname -a >"$result_dir/uname.txt"
    lscpu >"$result_dir/lscpu.txt"
    perf --version >"$result_dir/perf-version.txt"
    {
        sysctl kernel.perf_event_paranoid kernel.kptr_restrict
        printf 'cpu_online=%s\n' "$(< /sys/devices/system/cpu/online)"
        taskset -pc $$
    } >"$result_dir/perf-environment.txt" 2>&1
    if [[ -f "$project_root/build-profile-http3-jemalloc/CMakeCache.txt" ]]; then
        cp "$project_root/build-profile-http3-jemalloc/CMakeCache.txt" "$result_dir/CMakeCache-profile-server.txt"
    fi
}

run_case() {
    local case_name="$1"
    local case_dir="$result_dir/$case_name"
    local target_path expected_bytes streams method
    local -a request_options=()
    local client_status=0
    local perf_status=0
    local failed_requests=0
    local samples=0
    local svg_status=missing
    local sample_seconds=$((warmup_seconds + duration_seconds + 1))
    local client_watchdog=$((warmup_seconds + duration_seconds + drain_seconds + 15))
    local task_dir perf_tids
    local -a server_tids=()

    case "$case_name" in
        get-1k)
            target_path=/bench/1k
            expected_bytes=1024
            streams=8
            method=GET
            ;;
        get-64k)
            target_path=/bench/64k
            expected_bytes=65536
            streams=4
            method=GET
            ;;
        get-1m)
            target_path=/bench/1m
            expected_bytes=1048576
            streams=2
            method=GET
            ;;
        post-64k)
            target_path=/bench/echo
            expected_bytes=65536
            streams=4
            method=POST
            request_options=(--body "$result_dir/request-64k.bin")
            ;;
        *)
            fail "unsupported case: $case_name"
            ;;
    esac

    mkdir -p "$case_dir"
    wait_for_port_clear || fail "port $port is already in use; refusing to stop an unknown process"

    log "starting $case_name server (jemalloc, pacing=off)"
    taskset -c "$sut_cpus" "$server_bin" "$port" "$server_workers" \
        "$cert_file" "$key_file" off >"$case_dir/server.out" 2>"$case_dir/server.err" &
    active_server_pid=$!
    printf '%s\n' "$active_server_pid" >"$case_dir/server.pid"

    if ! wait_for_server; then
        echo "server failed readiness check" >>"$case_dir/server.err"
        stop_pid "$active_server_pid"
        active_server_pid=""
        fail "server failed readiness check for $case_name; see $case_dir/server.err"
    fi

    for task_dir in "/proc/$active_server_pid/task/"*; do
        [[ -d "$task_dir" ]] && server_tids+=("${task_dir##*/}")
    done
    [[ ${#server_tids[@]} -gt 0 ]] || fail "could not enumerate server threads for PID $active_server_pid"
    perf_tids="$(IFS=,; echo "${server_tids[*]}")"

    {
        printf 'case=%s\n' "$case_name"
        printf 'target_path=%s\n' "$target_path"
        printf 'method=%s\n' "$method"
        printf 'expected_bytes=%s\n' "$expected_bytes"
        printf 'streams_per_connection=%s\n' "$streams"
        printf 'server_pid=%s\n' "$active_server_pid"
        printf 'server_tids=%s\n' "$perf_tids"
        printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
    } >"$case_dir/case.env"

    log "recording $case_name at ${perf_frequency} Hz for ${sample_seconds}s"
    perf record --quiet --freq "$perf_frequency" --event "$perf_event" \
        --call-graph fp --tid "$perf_tids" \
        --output "$case_dir/perf.data" -- sleep "$sample_seconds" \
        >"$case_dir/perf-record.out" 2>"$case_dir/perf-record.err" &
    active_perf_pid=$!
    sleep 0.25
    if ! kill -0 "$active_perf_pid" 2>/dev/null; then
        wait "$active_perf_pid" || perf_status=$?
        active_perf_pid=""
        echo "$perf_status" >"$case_dir/perf.status"
        stop_pid "$active_server_pid"
        active_server_pid=""
        fail "perf record failed for $case_name; see $case_dir/perf-record.err"
    fi

    timeout --signal=INT --kill-after=10s "${client_watchdog}s" \
        taskset -c "$client_cpus" "$client_bin" \
        "https://localhost:$port$target_path" \
        --connect-to "127.0.0.1:$port" --insecure --pacing off \
        --mode closed --threads "$client_threads" --connections "$client_connections" \
        --streams "$streams" --warmup "${warmup_seconds}s" \
        --duration "${duration_seconds}s" --drain "${drain_seconds}s" \
        --method "$method" "${request_options[@]}" \
        --expect-status 200 --expect-bytes "$expected_bytes" \
        --json "$case_dir/client.json" \
        >"$case_dir/client.out" 2>"$case_dir/client.err" || client_status=$?
    echo "$client_status" >"$case_dir/client.status"

    wait "$active_perf_pid" || perf_status=$?
    active_perf_pid=""
    echo "$perf_status" >"$case_dir/perf.status"

    stop_pid "$active_server_pid"
    active_server_pid=""
    wait_for_port_clear || {
        echo "port $port did not clear after stopping $case_name" >&2
        overall_status=1
    }

    if [[ "$perf_status" -eq 0 && -s "$case_dir/perf.data" ]]; then
        if ! perf script --input "$case_dir/perf.data" >"$case_dir/perf.script" \
            2>"$case_dir/perf-script.err"; then
            overall_status=1
        fi
        if [[ -s "$case_dir/perf.script" ]] && \
            ! perl "$stackcollapse" "$case_dir/perf.script" >"$case_dir/perf.folded" \
                2>"$case_dir/stackcollapse.err"; then
            overall_status=1
        fi
        if [[ -s "$case_dir/perf.folded" ]]; then
            samples="$(awk '{ total += $NF } END { print total + 0 }' "$case_dir/perf.folded")"
            if perl "$flamegraph" --colors hot --hash --width 1600 \
                --title "http3_benchmark_server: $case_name (jemalloc, pacing off)" \
                --countname samples "$case_dir/perf.folded" >"$case_dir/flamegraph.svg" \
                2>"$case_dir/flamegraph.err"; then
                svg_status=ok
            else
                overall_status=1
            fi
        else
            overall_status=1
        fi
        perf report --input "$case_dir/perf.data" --stdio --no-children \
            --sort comm,dso,symbol --percent-limit 0.5 \
            >"$case_dir/perf.report.txt" 2>"$case_dir/perf-report.err" || overall_status=1
    else
        overall_status=1
    fi

    if [[ -s "$case_dir/client.json" ]]; then
        failed_requests="$(jq -r '.failed // 0' "$case_dir/client.json")"
    fi
    if [[ "$client_status" -ne 0 || "$failed_requests" -ne 0 ]]; then
        overall_status=1
    fi

    local started succeeded rps mibps
    started="$(jq -r '.started // 0' "$case_dir/client.json" 2>/dev/null || echo 0)"
    succeeded="$(jq -r '.succeeded // 0' "$case_dir/client.json" 2>/dev/null || echo 0)"
    rps="$(jq -r '.requests_per_second // 0' "$case_dir/client.json" 2>/dev/null || echo 0)"
    mibps="$(jq -r '.mib_per_second // 0' "$case_dir/client.json" 2>/dev/null || echo 0)"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$case_name" "$client_status" "$perf_status" "$started" "$succeeded" \
        "$failed_requests" "$rps" "$mibps" "$samples" "$svg_status" \
        >>"$result_dir/summary.csv"
    log "finished $case_name: client=$client_status failed=$failed_requests samples=$samples svg=$svg_status"
}

if [[ "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
[[ $# -eq 0 ]] || fail "unexpected arguments; use environment overrides or --help"
[[ $EUID -eq 0 ]] || fail "run this script with sudo"
[[ "$run_id" =~ ^[A-Za-z0-9._-]+$ ]] || fail "RUN_ID contains unsupported characters: $run_id"

for command in awk curl git jq lscpu perf perl readelf sha256sum ss sysctl taskset timeout; do
    command -v "$command" >/dev/null 2>&1 || fail "missing command: $command"
done
for executable in "$server_bin" "$client_bin" "$stackcollapse" "$flamegraph"; do
    [[ -x "$executable" ]] || fail "missing executable: $executable"
done
for input in "$cert_file" "$key_file"; do
    [[ -s "$input" ]] || fail "missing input file: $input"
done

require_positive_integer PORT "$port"
require_positive_integer SERVER_WORKERS "$server_workers"
require_positive_integer CLIENT_THREADS "$client_threads"
require_positive_integer CLIENT_CONNECTIONS "$client_connections"
require_positive_integer WARMUP_SECONDS "$warmup_seconds"
require_positive_integer DURATION_SECONDS "$duration_seconds"
require_positive_integer DRAIN_SECONDS "$drain_seconds"
require_positive_integer PERF_FREQUENCY "$perf_frequency"
((port <= 65535)) || fail "PORT must be at most 65535"
((client_threads <= client_connections)) || fail "CLIENT_THREADS cannot exceed CLIENT_CONNECTIONS"

taskset -c "$sut_cpus" true >/dev/null 2>&1 || fail "invalid or unavailable SUT_CPUS: $sut_cpus"
taskset -c "$client_cpus" true >/dev/null 2>&1 || fail "invalid or unavailable CLIENT_CPUS: $client_cpus"
port_is_listening && fail "port $port is already in use; refusing to stop an unknown process"

read -r -a selected_cases <<<"$cases_text"
[[ ${#selected_cases[@]} -gt 0 ]] || fail "CASES is empty"
for case_name in "${selected_cases[@]}"; do
    case "$case_name" in
        get-1k|get-64k|get-1m|post-64k) ;;
        *) fail "unsupported CASES value: $case_name" ;;
    esac
done

mkdir -p "$result_parent"
if [[ "${SUDO_UID:-0}" =~ ^[0-9]+$ && "${SUDO_UID:-0}" -ne 0 ]]; then
    chown "${SUDO_UID}:${SUDO_GID}" "$result_parent"
fi
[[ ! -e "$result_dir" ]] || fail "result directory already exists: $result_dir"
mkdir -p "$result_dir"
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

write_run_metadata
truncate -s 65536 "$result_dir/request-64k.bin"
printf 'case,client_status,perf_status,started,succeeded,failed,rps,mib_per_second,samples,svg\n' \
    >"$result_dir/summary.csv"

log "checking perf access"
if ! perf record --quiet --freq 99 --event "$perf_event" --call-graph fp \
    --output "$result_dir/perf-preflight.data" -- sleep 0.05 \
    >"$result_dir/perf-preflight.out" 2>"$result_dir/perf-preflight.err"; then
    fail "perf preflight failed; see $result_dir/perf-preflight.err"
fi

for case_name in "${selected_cases[@]}"; do
    run_case "$case_name"
done

printf 'finished_at=%s\n' "$(date --iso-8601=seconds)" >>"$result_dir/run.env"
restore_result_owner
trap - EXIT INT TERM

log "results: $result_dir"
log "summary: $result_dir/summary.csv"
exit "$overall_status"
