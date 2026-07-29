#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: verify_response.sh <plain|tls-h1|h2>" >&2
    exit 2
fi

mode="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http-benchmark-runtime"
output="$runtime_dir/verify-$mode.bin"
meta="$runtime_dir/verify-$mode.meta"
plain_port="${PLAIN_PORT:-18080}"
tls_port="${TLS_PORT:-18443}"

case "$mode" in
    plain)
        curl_args=(--http1.1 "http://127.0.0.1:$plain_port/bench/1k")
        expected_version="1.1"
        ;;
    tls-h1)
        curl_args=(--insecure --http1.1 "https://127.0.0.1:$tls_port/bench/1k")
        expected_version="1.1"
        ;;
    h2)
        curl_args=(--insecure --http2 "https://127.0.0.1:$tls_port/bench/1k")
        expected_version="2"
        ;;
    *)
        echo "unknown mode: $mode" >&2
        exit 2
        ;;
esac

curl --fail --silent --show-error \
    --output "$output" \
    --write-out '%{http_version} %{http_code} %{size_download}\n' \
    "${curl_args[@]}" >"$meta"

read -r actual_version status size <"$meta"
[[ "$actual_version" == "$expected_version" ]]
[[ "$status" == "200" ]]
[[ "$size" == "1024" ]]
[[ "$(sha256sum "$output" | cut -d' ' -f1)" == \
   "2edc986847e209b4016e141a6dc8716d3207350f416969382d431539bf292e4a" ]]

echo "verified mode=$mode version=$actual_version status=$status bytes=$size"
