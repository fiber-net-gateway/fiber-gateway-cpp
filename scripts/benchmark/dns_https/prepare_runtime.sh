#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../../.." && pwd)"
runtime_dir="$project_root/temp/dns-https-benchmark-runtime"

mkdir -p \
    "$runtime_dir/backend-a-client-body" \
    "$runtime_dir/backend-a-proxy" \
    "$runtime_dir/backend-b-client-body" \
    "$runtime_dir/backend-b-proxy" \
    "$runtime_dir/nginx-sut-client-body" \
    "$runtime_dir/nginx-sut-proxy"

cp "$script_dir/configs/db.dns-bench.test" \
    "$runtime_dir/db.dns-bench.test"

printf 'nameserver 127.0.0.53\noptions attempts:1 timeout:1\n' \
    >"$runtime_dir/resolv.conf"

test -s "$project_root/build/http3-demo/cert.pem"
test -s "$project_root/build/http3-demo/key.pem"

echo "$runtime_dir"
