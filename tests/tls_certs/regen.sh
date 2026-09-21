#!/usr/bin/env bash
# Regenerate the machine-generated TLS test material:
#   tests/TlsCertFixtures.h     certificate tree + private keys (PEM)
#   tests/TlsSignatureVectors.h signature KAT vectors
# The generated headers are committed; rerun only when the fixture tree
# changes. Requires python3-cryptography (2.8+).
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tests/tls_certs/gen_tls_certs.py
