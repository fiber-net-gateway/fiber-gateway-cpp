#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http-benchmark-runtime"
client="${HTTP3_CLIENT_BIN:-$project_root/build-benchmark-all/example/http3_benchmark_client}"
lite="${LITE_NGINX_BIN:-$project_root/build-benchmark-all/apps/lite_nginx}"
openresty="${OPENRESTY_BIN:-$project_root/temp/openresty-all-benchmark/nginx/nginx/sbin/nginx}"
backend="${BACKEND_BIN:-$project_root/build-benchmark-all/example/http_benchmark_backend}"
lite_config="${LITE_CONFIG:-$script_dir/configs/lite_nginx.conf}"
openresty_config="${OPENRESTY_CONFIG:-$script_dir/configs/openresty.conf}"
collect_cgroup="$script_dir/../http/collect_cgroup.sh"

repetitions="${REPETITIONS:-3}"
duration="${DURATION:-8s}"
warmup="${WARMUP:-2s}"
cooldown="${COOLDOWN:-1}"
connections="${CONNECTIONS:-2}"
streams="${STREAMS:-4}"
pacing="${PACING:-on}"
implementations="${IMPLEMENTATIONS:-lite openresty}"
case_filter="${CASE_FILTER:-}"
run_id="${RUN_ID:-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/all-benchmark-results/http3-$run_id}"
backend_cpus="${BACKEND_CPUS:-2}"
sut_cpus="${SUT_CPUS:-0,1}"
client_cpus="${CLIENT_CPUS:-3}"
tls_port="${TLS_PORT:-28443}"
backend_port="${BACKEND_PORT:-29001}"

backend_unit="allbench-h3-backend-$run_id"
active_sut_unit=""
overall_status=0

stop_unit() {
    local unit="$1"
    [[ -n "$unit" ]] || return
    systemctl --user stop "$unit" >/dev/null 2>&1 || true
}

cleanup() {
    stop_unit "$active_sut_unit"
    stop_unit "$backend_unit"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

wait_for_url() {
    local url="$1"
    for attempt in $(seq 1 100); do
        if /snap/bin/curl --http3-only --insecure --silent --fail \
            --output /dev/null "$url"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

case_settings() {
    method=GET
    body_options=()
    path=/bench/1k
    expected_bytes=1024
    case "$1" in
        H3-GET-1K) ;;
        H3-GET-64K)
            path=/bench/64k
            expected_bytes=65536
            ;;
        H3-GET-1M)
            path=/bench/1m
            expected_bytes=1048576
            ;;
        H3-POST-1M)
            method=POST
            path=/bench/echo
            expected_bytes=1048576
            body_options=(--method POST --body "$runtime_dir/request_1m.bin")
            ;;
        *)
            echo "unknown case: $1" >&2
            return 2
            ;;
    esac
    target_url="https://127.0.0.1:$tls_port$path"
}

start_sut() {
    local implementation="$1"
    local unit="$2"
    local command=()
    case "$implementation" in
        lite)
            command=("$lite" --config "$lite_config")
            ;;
        openresty)
            command=("$openresty" -p "$project_root/" -c "$openresty_config" -g 'daemon off;')
            ;;
        *)
            return 2
            ;;
    esac
    systemd-run --user --unit="$unit" --property="CPUAffinity=$sut_cpus" \
        --property=CPUAccounting=yes --property=MemoryAccounting=yes \
        --working-directory="$project_root" "${command[@]}" >/dev/null
    active_sut_unit="$unit"
    wait_for_url "https://127.0.0.1:$tls_port/bench/1k"
}

run_case() {
    local case_name="$1"
    local implementation="$2"
    local repetition="$3"
    local sequence="$4"
    local unit="allbench-h3-sut-$run_id-$sequence"
    local output_dir="$result_dir/runs/$case_name/rep-$repetition/$implementation"
    local gate_status=0
    local load_status=0
    mkdir -p "$output_dir"
    case_settings "$case_name" || return
    if ! start_sut "$implementation" "$unit"; then
        overall_status=1
        stop_unit "$unit"
        active_sut_unit=""
        return
    fi

    taskset -c "$client_cpus" "$client" "$target_url" --insecure \
        --pacing "$pacing" --threads 1 --connections 1 --streams 1 \
        --warmup 100ms --duration 300ms --drain 2s --timeout 5s \
        --expect-status 200 --expect-bytes "$expected_bytes" \
        "${body_options[@]}" --json "$output_dir/gate.json" \
        >"$output_dir/gate.out" 2>"$output_dir/gate.err" || gate_status=$?
    if [[ "$gate_status" -ne 0 ]] || \
       [[ "$(jq -r '.failed' "$output_dir/gate.json" 2>/dev/null)" != 0 ]]; then
        overall_status=1
    fi
    printf '%s\n' "$gate_status" >"$output_dir/gate.status"

    {
        printf 'case=%s\n' "$case_name"
        printf 'implementation=%s\n' "$implementation"
        printf 'repetition=%s\n' "$repetition"
        printf 'method=%s\n' "$method"
        printf 'target_url=%s\n' "$target_url"
        printf 'expected_bytes=%s\n' "$expected_bytes"
        printf 'connections=%s\n' "$connections"
        printf 'streams=%s\n' "$streams"
        printf 'pacing=%s\n' "$pacing"
    } >"$output_dir/meta.env"

    nstat -az >"$output_dir/nstat-before.txt"
    "$collect_cgroup" "$unit" "$output_dir/sut-before" >/dev/null 2>&1 || true
    /usr/bin/time -v -o "$output_dir/client.time" \
        taskset -c "$client_cpus" "$client" "$target_url" --insecure \
        --pacing "$pacing" --threads 1 --connections "$connections" \
        --streams "$streams" --warmup "$warmup" --duration "$duration" \
        --drain 5s --timeout 10s --expect-status 200 \
        --expect-bytes "$expected_bytes" "${body_options[@]}" \
        --json "$output_dir/client.json" \
        >"$output_dir/client.out" 2>"$output_dir/client.err" || load_status=$?
    "$collect_cgroup" "$unit" "$output_dir/sut-after" >/dev/null 2>&1 || true
    nstat -az >"$output_dir/nstat-after.txt"
    printf '%s\n' "$load_status" >"$output_dir/client.status"
    journalctl --user --unit="$unit" --no-pager \
        >"$output_dir/sut-journal.log" 2>&1 || true

    if [[ "$load_status" -ne 0 ]] || \
       [[ "$(jq -r '.failed' "$output_dir/client.json" 2>/dev/null)" != 0 ]]; then
        overall_status=1
    fi
    stop_unit "$unit"
    active_sut_unit=""
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    [[ "$cooldown" == 0 ]] || sleep "$cooldown"
}

for executable in "$client" "$lite" "$openresty" "$backend"; do
    [[ -x "$executable" ]] || { echo "missing executable: $executable" >&2; exit 1; }
done
"$script_dir/../http/prepare_runtime.sh" >/dev/null
mkdir -p "$result_dir/runs"

{
    printf 'run_id=%s\n' "$run_id"
    printf 'repetitions=%s\n' "$repetitions"
    printf 'duration=%s\n' "$duration"
    printf 'warmup=%s\n' "$warmup"
    printf 'connections=%s\n' "$connections"
    printf 'streams=%s\n' "$streams"
    printf 'pacing=%s\n' "$pacing"
    printf 'implementations=%s\n' "$implementations"
    printf 'backend_cpus=%s\n' "$backend_cpus"
    printf 'sut_cpus=%s\n' "$sut_cpus"
    printf 'client_cpus=%s\n' "$client_cpus"
    printf 'git_commit=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
} >"$result_dir/run.env"
uname -a >"$result_dir/uname.txt"
lscpu >"$result_dir/lscpu.txt"
git -C "$project_root" status --short >"$result_dir/git-status.txt"
sha256sum "$client" "$lite" "$openresty" "$backend" >"$result_dir/binary-sha256.txt"
"$openresty" -V >"$result_dir/openresty-version.txt" 2>&1

systemd-run --user --unit="$backend_unit" --property="CPUAffinity=$backend_cpus" \
    --property=CPUAccounting=yes --property=MemoryAccounting=yes \
    --working-directory="$project_root" "$backend" "$backend_port" >/dev/null
for attempt in $(seq 1 100); do
    curl --silent --fail --output /dev/null \
        "http://127.0.0.1:$backend_port/bench/1k" && break
    sleep 0.1
done

sequence=0
for case_name in H3-GET-1K H3-GET-64K H3-GET-1M H3-POST-1M; do
    [[ -z "$case_filter" || "$case_name" == *"$case_filter"* ]] || continue
    for repetition in $(seq 1 "$repetitions"); do
        read -r -a ordered <<<"$implementations"
        if (( repetition % 2 == 0 )); then
            ordered=("${ordered[1]}" "${ordered[0]}")
        fi
        for implementation in "${ordered[@]}"; do
            sequence=$((sequence + 1))
            echo "running case=$case_name rep=$repetition implementation=$implementation"
            run_case "$case_name" "$implementation" "$repetition" "$sequence"
        done
    done
done

stop_unit "$backend_unit"
trap - EXIT INT TERM
echo "$result_dir"
exit "$overall_status"
