#!/usr/bin/env bash
# Anti-regression guard (feature/tls/10 §10): fiber_lib links BoringSSL's
# crypto library only. The ssl library lives on in tests/ as the differential
# peer (待拍板 A's recommended shape) — a src/ or include/ file pulling in
# <openssl/ssl.h> would silently reintroduce the ssl dependency.
set -euo pipefail
cd "$(dirname "$0")/.."

if grep -rn --include='*.h' --include='*.cpp' '#include <openssl/ssl.h>' src include; then
    echo "FAIL: <openssl/ssl.h> included under src/ or include/ — fiber_lib is crypto-only (feature/tls/10 §10)" >&2
    exit 1
fi
echo "ok: no <openssl/ssl.h> includes under src/ or include/"
