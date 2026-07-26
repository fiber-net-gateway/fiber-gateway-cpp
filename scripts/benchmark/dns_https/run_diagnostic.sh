#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/dns-https-benchmark-runtime"
coredns="$project_root/temp/coredns-1.14.6/coredns"
nginx="$project_root/temp/nginx-install/sbin/nginx"
lite_nginx="$project_root/build-bench/apps/lite_nginx"
origin="$project_root/build-bench/example/http_benchmark_backend"
h2load="$project_root/temp/http-bench-tools/root/usr/bin/h2load"
h2load_library_path="$project_root/temp/http-bench-tools/root/usr/lib/x86_64-linux-gnu"

repetitions="${REPETITIONS:-3}"
duration="${DURATION:-15}"
warmup="${WARMUP:-5}"
cooldown="${COOLDOWN:-3}"
clients="${CLIENTS:-128}"
load_threads="${LOAD_THREADS:-4}"
implementations="${IMPLEMENTATIONS:-lite nginx}"
run_id="${RUN_ID:-diagnostic-$(date +%Y%m%dT%H%M%S)}"
result_dir="${RESULT_DIR:-$project_root/temp/dns-https-benchmark-results/$run_id}"

sut_cpus="${SUT_CPUS:-0,2}"
client_cpus="${CLIENT_CPUS:-6,8,10,12}"
backend_cpus="${BACKEND_CPUS:-14,16}"
dns_cpu="${DNS_CPU:-18}"

if [[ "${FIBER_DNS_HTTPS_IN_NAMESPACE:-0}" != 1 ]]; then
    "$script_dir/prepare_runtime.sh" >/dev/null
    exec unshare \
        --user --map-root-user \
        --net --mount \
        --pid --fork --mount-proc \
        env FIBER_DNS_HTTPS_IN_NAMESPACE=1 \
        "$0" "$@"
fi

mkdir -p "$result_dir/runs/P01-DNS-HTTPS-KEEPALIVE"
ip link set lo up
sysctl -q -w net.ipv4.ip_unprivileged_port_start=0
mount --bind "$runtime_dir/resolv.conf" /etc/resolv.conf
cd "$project_root"

dns_pid=""
origin_pid=""
backend_pid=""
sut_time_pid=""
sut_pid=""

stop_process() {
    local pid="$1"
    local signal="${2:-TERM}"
    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
        kill "-$signal" "$pid" >/dev/null 2>&1 || true
        for attempt in $(seq 1 100); do
            if ! kill -0 "$pid" >/dev/null 2>&1; then
                break
            fi
            sleep 0.05
        done
        if kill -0 "$pid" >/dev/null 2>&1; then
            kill -KILL "$pid" >/dev/null 2>&1 || true
        fi
    fi
    if [[ -n "$pid" ]]; then
        wait "$pid" >/dev/null 2>&1 || true
    fi
}

stop_sut() {
    stop_process "$sut_pid"
    if [[ -n "$sut_time_pid" ]]; then
        wait "$sut_time_pid" >/dev/null 2>&1 || true
    fi
    sut_pid=""
    sut_time_pid=""
}

cleanup() {
    stop_sut
    stop_process "$backend_pid"
    stop_process "$origin_pid"
    stop_process "$dns_pid"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

wait_http() {
    local url="$1"
    local max_attempts="${2:-200}"
    for attempt in $(seq 1 "$max_attempts"); do
        if curl --noproxy '*' --max-time 1 --silent --fail \
            --output /dev/null "$url"; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

snapshot_dns_metrics() {
    local output="$1"
    curl --noproxy '*' --max-time 2 --silent --fail \
        http://127.0.0.53:9153/metrics >"$output"
}

start_base_stack() {
    taskset -c "$dns_cpu" "$coredns" \
        -conf scripts/benchmark/dns_https/configs/Corefile \
        >"$result_dir/coredns.log" 2>&1 &
    dns_pid=$!
    wait_http http://127.0.0.53:18053/health
    dig +short backend-long.dns-bench.test A \
        >"$result_dir/dns-preflight.txt"
    rg -q '^127\.0\.0\.1$' "$result_dir/dns-preflight.txt"

    taskset -c "$backend_cpus" "$origin" 19001 \
        >"$result_dir/origin.log" 2>&1 &
    origin_pid=$!
    wait_http http://127.0.0.1:19001/bench/1k
    curl --noproxy '*' --max-time 2 --silent --fail \
        http://127.0.0.1:19001/bench/1k \
        --output "$result_dir/origin-1k.body"
    sha256sum "$result_dir/origin-1k.body" \
        >"$result_dir/origin-1k.sha256"

    : >"$runtime_dir/backend-a-error.log"
    taskset -c "$backend_cpus" "$nginx" \
        -p "$project_root/" \
        -c scripts/benchmark/dns_https/configs/nginx_backend_a.conf \
        -g 'daemon off;' \
        >"$result_dir/backend-a.stdout.log" 2>&1 &
    backend_pid=$!
    for attempt in $(seq 1 200); do
        if curl --noproxy '*' --max-time 1 --silent --insecure --fail \
            --resolve backend-long.dns-bench.test:19443:127.0.0.1 \
            --output /dev/null \
            https://backend-long.dns-bench.test:19443/bench/1k; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

start_sut() {
    local implementation="$1"
    local output_dir="$2"
    local command=()

    case "$implementation" in
        lite)
            command=(
                "$lite_nginx"
                --config scripts/benchmark/dns_https/configs/lite_nginx_sut.conf
            )
            ;;
        nginx)
            command=(
                "$nginx"
                -p "$project_root/"
                -c scripts/benchmark/dns_https/configs/nginx_sut.conf
                -g 'daemon off;'
            )
            ;;
        *)
            return 2
            ;;
    esac

    : >"$runtime_dir/nginx-sut-error.log"
    /usr/bin/time -v -o "$output_dir/sut.time" \
        taskset -c "$sut_cpus" "${command[@]}" \
        >"$output_dir/sut.stdout.log" 2>"$output_dir/sut.stderr.log" &
    sut_time_pid=$!

    for attempt in $(seq 1 200); do
        sut_pid="$(pgrep -P "$sut_time_pid" | head -n 1 || true)"
        if [[ -n "$sut_pid" ]]; then
            break
        fi
        sleep 0.01
    done
    [[ -n "$sut_pid" ]]

    wait_http http://127.0.0.1:18080/bench/1k
}

verify_sut() {
    local output_dir="$1"
    curl --noproxy '*' --max-time 3 --silent --fail \
        --dump-header "$output_dir/verify.headers" \
        --output "$output_dir/verify.body" \
        http://127.0.0.1:18080/bench/1k
    cmp "$result_dir/origin-1k.body" "$output_dir/verify.body"
    rg -i -q '^x-benchmark-backend: A' "$output_dir/verify.headers"
    rg -i -q \
        '^x-benchmark-upstream-sni: backend-long\.dns-bench\.test' \
        "$output_dir/verify.headers"
}

snapshot_process_cpu() {
    local root_pid="$1"
    local output="$2"
    local clock_ticks
    local pending=("$root_pid")
    local processes=()
    local index=0
    local user_ticks=0
    local system_ticks=0

    clock_ticks="$(getconf CLK_TCK)"
    while (( index < ${#pending[@]} )); do
        local process_pid="${pending[index]}"
        index=$((index + 1))
        if [[ ! -d "/proc/$process_pid" ]]; then
            continue
        fi
        processes+=("$process_pid")
        while IFS= read -r child_pid; do
            [[ -n "$child_pid" ]] && pending+=("$child_pid")
        done < <(pgrep -P "$process_pid" || true)
    done

    for process_pid in "${processes[@]}"; do
        local task_stat
        for task_stat in /proc/"$process_pid"/task/*/stat; do
            if [[ -r "$task_stat" ]]; then
                read -r task_user task_system < <(
                    awk '{ print $14, $15 }' "$task_stat"
                )
                user_ticks=$((user_ticks + task_user))
                system_ticks=$((system_ticks + task_system))
            fi
        done
    done

    awk -v user="$user_ticks" -v sys="$system_ticks" -v hz="$clock_ticks" \
        'BEGIN {
            printf "usage_usec %d\n", (user + sys) * 1000000 / hz;
            printf "user_usec %d\n", user * 1000000 / hz;
            printf "system_usec %d\n", sys * 1000000 / hz;
            print "nr_throttled 0";
            print "throttled_usec 0";
        }' >"$output"
}

run_one() {
    local implementation="$1"
    local repetition="$2"
    local output_dir="$result_dir/runs/P01-DNS-HTTPS-KEEPALIVE/rep-$repetition/$implementation"
    local h2load_status=0

    mkdir -p "$output_dir"
    snapshot_dns_metrics "$output_dir/dns-before.prom"
    start_sut "$implementation" "$output_dir"
    verify_sut "$output_dir"

    {
        printf 'case=P01-DNS-HTTPS-KEEPALIVE\n'
        printf 'implementation=%s\n' "$implementation"
        printf 'repetition=%s\n' "$repetition"
        printf 'duration_seconds=%s\n' "$duration"
        printf 'warmup_seconds=%s\n' "$warmup"
        printf 'clients=%s\n' "$clients"
        printf 'streams_per_client=1\n'
        printf 'load_threads=%s\n' "$load_threads"
        printf 'target_url=http://127.0.0.1:18080/bench/1k\n'
        printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
    } >"$output_dir/meta.env"

    env LD_LIBRARY_PATH="$h2load_library_path" \
        taskset -c "$client_cpus" "$h2load" \
        --h1 \
        -t "$load_threads" \
        -c "$clients" \
        -m 1 \
        -D "${duration}s" \
        --warm-up-time="${warmup}s" \
        -N 10s \
        --log-file="$output_dir/requests.tsv" \
        http://127.0.0.1:18080/bench/1k \
        >"$output_dir/h2load.out" 2>"$output_dir/h2load.err" &
    local h2load_pid=$!
    sleep "$warmup"
    snapshot_process_cpu "$sut_pid" "$output_dir/sut-before.cpu.stat"
    wait "$h2load_pid" || h2load_status=$?
    snapshot_process_cpu "$sut_pid" "$output_dir/sut-after.cpu.stat"
    printf '%s\n' "$h2load_status" >"$output_dir/h2load.status"

    verify_sut "$output_dir"
    snapshot_dns_metrics "$output_dir/dns-after.prom"
    if ! kill -0 "$sut_pid" >/dev/null 2>&1; then
        printf 'SUT exited during load\n' >&2
        return 1
    fi
    {
        printf 'ActiveState=active\n'
        printf 'Result=success\n'
    } >"$output_dir/sut-settled.unit"
    stop_sut
    cp "$runtime_dir/nginx-sut-error.log" \
        "$output_dir/nginx-error.log" 2>/dev/null || true
    [[ "$h2load_status" = 0 ]]
}

start_base_stack

{
    printf 'run_id=%s\n' "$run_id"
    printf 'environment=WSL-user-network-namespace\n'
    printf 'repetitions=%s\n' "$repetitions"
    printf 'duration_seconds=%s\n' "$duration"
    printf 'warmup_seconds=%s\n' "$warmup"
    printf 'clients=%s\n' "$clients"
    printf 'implementations=%q\n' "$implementations"
    printf 'sut_cpus=%s\n' "$sut_cpus"
    printf 'client_cpus=%s\n' "$client_cpus"
    printf 'backend_cpus=%s\n' "$backend_cpus"
    printf 'dns_cpu=%s\n' "$dns_cpu"
    printf 'git_commit=%s\n' "$(git rev-parse HEAD)"
    printf 'started_at=%s\n' "$(date --iso-8601=seconds)"
} >"$result_dir/run.env"

uname -a >"$result_dir/uname.txt"
lscpu >"$result_dir/lscpu.txt"
git status --short >"$result_dir/git-status.txt"
sha256sum "$lite_nginx" "$nginx" "$origin" "$coredns" \
    >"$result_dir/binary-sha256.txt"
"$nginx" -V >"$result_dir/nginx-version.txt" 2>&1
"$coredns" -version >"$result_dir/coredns-version.txt" 2>&1
env LD_LIBRARY_PATH="$h2load_library_path" "$h2load" --version \
    >"$result_dir/h2load-version.txt" 2>&1

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
        run_one "$implementation" "$repetition"
        sleep "$cooldown"
    done
done

snapshot_dns_metrics "$result_dir/dns-final.prom"
cp "$runtime_dir/db.dns-bench.test" "$result_dir/db.dns-bench.test"
cp scripts/benchmark/dns_https/configs/lite_nginx_sut.conf \
    "$result_dir/lite_nginx_sut.conf"
cp scripts/benchmark/dns_https/configs/nginx_sut.conf \
    "$result_dir/nginx_sut.conf"
cp scripts/benchmark/dns_https/configs/nginx_backend_a.conf \
    "$result_dir/nginx_backend_a.conf"

scripts/benchmark/http/summarize.py "$result_dir"
printf '%s\n' "$result_dir"
