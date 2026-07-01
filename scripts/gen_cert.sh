#!/usr/bin/env bash
#
# Generate a self-signed TLS certificate for local lite_nginx testing.
#
# Defaults write to build/http3-demo/ so the bundled config
# (apps/lite_nginx/conf/lite_nginx.conf) picks them up without edits:
#
#     certificate     build/http3-demo/cert.pem;
#     certificate_key build/http3-demo/key.pem;
#
# Usage:
#   scripts/gen_cert.sh                       # default paths
#   scripts/gen_cert.sh out/dir               # custom dir, cert.pem + key.pem
#   scripts/gen_cert.sh out/dir cert.pem key.pem
#   scripts/gen_cert.sh -d example.com ...    # override subject / SAN
#
# Options:
#   -d DOMAIN   Common Name + SAN entry (repeatable). Defaults: localhost + 127.0.0.1
#   -h          Print this help.
#
set -euo pipefail

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

domains=()
while getopts "d:h" opt; do
    case "$opt" in
        d) domains+=("$OPTARG") ;;
        h) usage ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))

# Resolve project root (this script lives in <root>/scripts/).
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"

out_dir="$project_root/build/http3-demo"
cert_name="cert.pem"
key_name="key.pem"
if [[ $# -ge 1 ]]; then out_dir="$1"; fi
if [[ $# -ge 2 ]]; then cert_name="$2"; fi
if [[ $# -ge 3 ]]; then key_name="$3"; fi

out_dir="$(cd -P "$out_dir" 2>/dev/null && pwd || mkdir -p "$out_dir" && cd "$out_dir" && pwd)"
cert_path="$out_dir/$cert_name"
key_path="$out_dir/$key_name"

# Default identities cover localhost + loopback for browser/curl testing.
if [[ ${#domains[@]} -eq 0 ]]; then
    domains=("localhost" "127.0.0.1")
fi

if ! command -v openssl >/dev/null 2>&1; then
    echo "error: openssl not found on PATH" >&2
    exit 1
fi

# Build SAN entries: DNS: for names, IP: for IPv4 literals.
san_entries=()
subject_alt_names=""
for d in "${domains[@]}"; do
    if [[ $d =~ ^[0-9]+(\.[0-9]+){3}$ ]]; then
        san_entries+=("IP:$d")
    else
        san_entries+=("DNS:$d")
    fi
done
subject_alt_names="$(IFS=,; echo "${san_entries[*]}")"

# First non-IP entry is the Common Name; fall back to the first entry.
common_name="${domains[0]}"
for d in "${domains[@]}"; do
    if [[ ! $d =~ ^[0-9]+(\.[0-9]+){3}$ ]]; then
        common_name="$d"
        break
    fi
done

echo "Generating self-signed certificate:"
echo "  dir:    $out_dir"
echo "  cert:   $cert_path"
echo "  key:    $key_path"
echo "  CN:     $common_name"
echo "  SAN:    $subject_alt_names"

# -addext requires openssl >= 1.1.1. If unavailable, fall back to a
# temporary openssl.cnf with the SAN baked in via the [alt_names] section.
if ! openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$key_path" -out "$cert_path" \
        -days 825 -sha256 \
        -subj "/CN=$common_name" \
        -addext "subjectAltName=$subject_alt_names" \
        -addext "basicConstraints=CA:FALSE" \
        -addext "keyUsage=digitalSignature,keyEncipherment" \
        -addext "extendedKeyUsage=serverAuth" 2>/dev/null; then

    tmp_cnf="$(mktemp)"
    trap 'rm -f "$tmp_cnf"' EXIT

    {
        echo "[req]"
        echo "distinguished_name=req_distinguished_name"
        echo "x509_extensions=v3_req"
        echo "prompt=no"
        echo "[req_distinguished_name]"
        echo "CN=$common_name"
        echo "[v3_req]"
        echo "subjectAltName=$subject_alt_names"
        echo "basicConstraints=CA:FALSE"
        echo "keyUsage=digitalSignature,keyEncipherment"
        echo "extendedKeyUsage=serverAuth"
    } > "$tmp_cnf"

    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$key_path" -out "$cert_path" \
        -days 825 -sha256 \
        -config "$tmp_cnf"
fi

chmod 600 "$key_path"
chmod 644 "$cert_path"

echo "Done. Point lite_nginx at it (defaults match conf/lite_nginx.conf):"
echo "  certificate     $cert_path"
echo "  certificate_key $key_path"
