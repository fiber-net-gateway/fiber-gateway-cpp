#!/usr/bin/env bash
#
# Download and build nginx for local HTTP/1.1, HTTP/2, and HTTP/3 testing.
#
# Usage:
#   cmake -S . -B build  # populate zlib under temp/_deps
#   scripts/build_nginx.sh
#
# Optional environment variables:
#   CC      C compiler passed to nginx's configure script.
#   JOBS    Number of parallel make jobs (defaults to the available CPU count).
#
set -euo pipefail

nginx_version="1.31.3"
nginx_url="https://nginx.org/download/nginx-${nginx_version}.tar.gz"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
temp_dir="$project_root/temp"
archive_path="$temp_dir/nginx-${nginx_version}.tar.gz"
source_dir="$temp_dir/nginx-${nginx_version}"
install_dir="$temp_dir/nginx-install"
cert_dir="$project_root/build/http3-demo"
cert_path="$cert_dir/cert.pem"
key_path="$cert_dir/key.pem"
boringssl_source_dir="$temp_dir/_deps/boringssl-src"
boringssl_build_dir="$temp_dir/_deps/boringssl-build"
zlib_source_dir="$temp_dir/_deps/zlib-src"

fail() {
    echo "error: $*" >&2
    exit 1
}

find_c_compiler() {
    if [[ -n "${CC:-}" ]]; then
        command -v "$CC" >/dev/null 2>&1 || fail "CC is not executable: $CC"
        command -v "$CC"
        return
    fi

    local candidate
    local candidates=(cc gcc clang)
    for candidate in /usr/bin/clang-[0-9]*; do
        candidates+=("$candidate")
    done

    for candidate in "${candidates[@]}"; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return
        fi
    done

    fail "no C compiler found; install gcc/clang or set CC"
}

download_nginx() {
    local partial_path="$archive_path.part"

    echo "Downloading $nginx_url"
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --output "$partial_path" "$nginx_url"
    elif command -v wget >/dev/null 2>&1; then
        wget --output-document="$partial_path" "$nginx_url"
    else
        fail "curl or wget is required to download nginx"
    fi

    tar -tzf "$partial_path" >/dev/null || fail "downloaded archive is not a valid gzip tarball"
    mv "$partial_path" "$archive_path"
}

command -v make >/dev/null 2>&1 || fail "make is required to build nginx"
command -v tar >/dev/null 2>&1 || fail "tar is required to extract nginx"

c_compiler="$(find_c_compiler)"
if [[ -n "${JOBS:-}" ]]; then
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "JOBS must be a positive integer"
    make_jobs="$JOBS"
elif command -v nproc >/dev/null 2>&1; then
    make_jobs="$(nproc)"
else
    make_jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
fi

mkdir -p "$temp_dir"
if [[ ! -f "$archive_path" ]] || ! tar -tzf "$archive_path" >/dev/null 2>&1; then
    download_nginx
else
    echo "Using cached archive $archive_path"
fi

if [[ ! -x "$source_dir/configure" ]]; then
    echo "Extracting nginx sources to $source_dir"
    tar -xzf "$archive_path" -C "$temp_dir"
fi
[[ -x "$source_dir/configure" ]] || fail "nginx configure script was not extracted"
[[ -f "$zlib_source_dir/zlib.h" && -x "$zlib_source_dir/configure" ]] ||
    fail "zlib sources not found; run 'cmake -S . -B build' from the project root first"

echo "Configuring nginx with $c_compiler"
cd "$source_dir"
configure_args=(
    "--prefix=$install_dir"
    "--error-log-path=$install_dir/logs/error.log"
    "--with-cc=$c_compiler"
    --with-http_ssl_module
    --with-http_v2_module
    --with-http_v3_module
    "--with-zlib=$zlib_source_dir"
    --without-http_rewrite_module
)

if [[ -f "$boringssl_source_dir/include/openssl/ssl.h" &&
      -f "$boringssl_build_dir/libssl.a" &&
      -f "$boringssl_build_dir/libcrypto.a" ]]; then
    echo "Using the project's BoringSSL build for QUIC TLS"
    configure_args+=(
        "--with-cc-opt=-I$boringssl_source_dir/include"
        "--with-ld-opt=-L$boringssl_build_dir -Wl,--no-as-needed -lstdc++"
    )
else
    echo "Project BoringSSL build not found; using the system OpenSSL development library"
fi

./configure "${configure_args[@]}"

echo "Building nginx with $make_jobs parallel job(s)"
make -j"$make_jobs"
make install

if [[ ! -s "$cert_path" || ! -s "$key_path" ]]; then
    "$script_dir/gen_cert.sh"
else
    echo "Using existing certificate files in $cert_dir"
fi

mkdir -p "$install_dir/logs"

nginx_bin="$install_dir/sbin/nginx"
"$nginx_bin" -t -p "$project_root/" -c scripts/nginx.conf

echo
echo "nginx $nginx_version is ready. From the project root, run:"
echo "  temp/nginx-install/sbin/nginx -p \"$project_root/\" -c scripts/nginx.conf"
echo "Stop it with:"
echo "  temp/nginx-install/sbin/nginx -p \"$project_root/\" -c scripts/nginx.conf -s stop"
