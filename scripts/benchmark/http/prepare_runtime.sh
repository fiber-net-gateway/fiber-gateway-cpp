#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/http-benchmark-runtime"

mkdir -p \
    "$runtime_dir/client-body" \
    "$runtime_dir/proxy"
truncate -s 65536 "$runtime_dir/request_64k.bin"
truncate -s 1048576 "$runtime_dir/request_1m.bin"

test -s "$project_root/build/http3-demo/cert.pem"
test -s "$project_root/build/http3-demo/key.pem"

echo "$runtime_dir"
