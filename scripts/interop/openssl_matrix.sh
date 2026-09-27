#!/usr/bin/env bash
# TLS interop matrix: the in-tree TLS stack (build/example/tls_probe) against
# the system OpenSSL.
#
#   Part 1 (focus): our CLIENT -> `openssl s_server`
#   Part 2:         `openssl s_client` -> our SERVER (tls_probe --listen)
#
# Every case runs against a fresh OpenSSL process on a free loopback port,
# with a PKI generated into a temp dir. A case passes when the outcome
# matches its expectation (ok / fail); ok cases also check the negotiated
# protocol and cipher as OpenSSL reports them.
#
# Usage: scripts/interop/openssl_matrix.sh [--probe PATH] [--keep] [--filter REGEX] [--verbose]
# Exit status: 0 = every case matched its expectation, 1 = mismatches.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE="${REPO_ROOT}/build/example/tls_probe"
KEEP=0
FILTER=""
VERBOSE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --probe) PROBE="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --filter) FILTER="$2"; shift 2 ;;
        --verbose) VERBOSE=1; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -x "${PROBE}" ]] || { echo "tls_probe not found at ${PROBE} (cmake --build build --target tls_probe)" >&2; exit 2; }
command -v openssl >/dev/null || { echo "openssl not on PATH" >&2; exit 2; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tls-interop.XXXXXX")"
cleanup() {
    jobs -p | xargs -r kill 2>/dev/null
    if [[ ${KEEP} -eq 1 ]]; then echo "work dir kept: ${WORK}"; else rm -rf "${WORK}"; fi
}
trap cleanup EXIT
cd "${WORK}" || exit 2

echo "OpenSSL: $(openssl version)"
echo "probe:   ${PROBE}"
echo "work:    ${WORK}"

# ---------------------------------------------------------------- PKI

q() { "$@" >/dev/null 2>&1 || { echo "PKI step failed: $*" >&2; exit 2; }; }

# Every certificate is issued through `openssl ca` with a notBefore one day in
# the past: certificates stamped "now" can read as not-yet-valid when the
# wall clock steps backwards (host time sync on VMs/WSL2 does this, by
# seconds), and OpenSSL 3.0's `x509 -req` cannot set a start date.
START="$(date -u -d '-1 day' +%Y%m%d%H%M%SZ)"
END="$(date -u -d '+30 days' +%Y%m%d%H%M%SZ)"
mkdir -p cadb && : > cadb/index.txt && echo 1000 > cadb/serial
cat > ca.cnf <<EOF
[ca]
default_ca = interop
[interop]
dir = ${WORK}/cadb
database = \$dir/index.txt
serial = \$dir/serial
new_certs_dir = \$dir
default_md = sha256
policy = pol
copy_extensions = none
unique_subject = no
[pol]
commonName = supplied
EOF
cat > root.ext <<'EOF'
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
EOF
cat > ca.ext <<'EOF'
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always
EOF
cat > server.ext <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,DNS:interop.test,IP:127.0.0.1
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always
EOF
cat > client.ext <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always
EOF

sign() { # sign CSR CA-cert CA-key ext out [start end]
    local csr=$1 cacert=$2 cakey=$3 ext=$4 out=$5 start=${6:-${START}} end=${7:-${END}}
    q openssl ca -batch -notext -config ca.cnf -cert "${cacert}" -keyfile "${cakey}" -in "${csr}" -out "${out}" \
        -extfile "${ext}" -startdate "${start}" -enddate "${end}"
}
self_signed_root() { # name cn
    q openssl req -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=$2"
    q openssl ca -batch -notext -config ca.cnf -selfsign -keyfile "$1.key" -in "$1.csr" -out "$1.pem" \
        -extfile root.ext -startdate "${START}" -enddate "${END}"
}

# Root and intermediates.
self_signed_root root "Interop Root"
q openssl req -newkey rsa:2048 -nodes -keyout int.key -out int.csr -subj "/CN=Interop Intermediate"
sign int.csr root.pem root.key ca.ext int.pem
q openssl req -newkey rsa:2048 -nodes -keyout int2.key -out int2.csr -subj "/CN=Interop Intermediate 2"
sign int2.csr int.pem int.key ca.ext int2.pem

# Server leaves (signed by the intermediate) and their chain files.
leaf() { # name keygen-args...
    local name=$1; shift
    q openssl genpkey "$@" -out "${name}.key"
    q openssl req -new -key "${name}.key" -out "${name}.csr" -subj "/CN=localhost"
    sign "${name}.csr" int.pem int.key server.ext "${name}.pem"
    cat "${name}.pem" int.pem > "${name}.chain.pem"
}
leaf rsa2048 -algorithm RSA -pkeyopt rsa_keygen_bits:2048
leaf rsa4096 -algorithm RSA -pkeyopt rsa_keygen_bits:4096
leaf p256 -algorithm EC -pkeyopt ec_paramgen_curve:P-256
leaf p384 -algorithm EC -pkeyopt ec_paramgen_curve:P-384
leaf ed25519 -algorithm ED25519
leaf rsapss -algorithm RSA-PSS -pkeyopt rsa_keygen_bits:2048

# Long chain: root -> int -> int2 -> leaf.
q openssl req -new -key rsa2048.key -out long.csr -subj "/CN=localhost"
sign long.csr int2.pem int2.key server.ext long.pem
cat int2.pem int.pem > long.extra.pem
# The root sent too, and a 6-entry chain (repeated intermediate + root).
cat int.pem root.pem > withroot.extra.pem
cat int.pem int.pem int.pem int.pem root.pem > six.extra.pem

# Expired leaf (validity entirely in the past).
q openssl req -new -key rsa2048.key -out expired.csr -subj "/CN=localhost"
sign expired.csr int.pem int.key server.ext expired.pem 20200101000000Z 20200201000000Z
cat expired.pem int.pem > expired.chain.pem

# An unrelated CA (untrusted-root cases) and a client certificate (mTLS).
self_signed_root other "Other Root"
q openssl req -newkey rsa:2048 -nodes -keyout client.key -out client.csr -subj "/CN=interop client"
sign client.csr int.pem int.key client.ext client.pem
cat client.pem int.pem > client.chain.pem

# Bulk payload for -WWW.
head -c 1048576 /dev/urandom > big.bin

# ---------------------------------------------------------------- harness

PASS=0
FAIL=0
RESULTS=()

free_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'
}

wait_listen() { # port
    local i
    for i in $(seq 1 60); do
        ss -ltn "sport = :$1" 2>/dev/null | grep -q LISTEN && return 0
        sleep 0.05
    done
    return 1
}

selected() { # name
    [[ -z "${FILTER}" ]] || [[ "$1" =~ ${FILTER} ]]
}

record() { # name verdict detail
    local name=$1 verdict=$2 detail=$3
    if [[ "${verdict}" == PASS ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        # Keep this case's logs for diagnosis (the work dir survives with --keep).
        local dir="${WORK}/failed/$((PASS + FAIL))-${name//[^A-Za-z0-9._-]/_}"
        mkdir -p "${dir}" && cp "${WORK}"/case.* "${dir}/" 2>/dev/null
    fi
    RESULTS+=("$(printf '%-4s  %-58s %s' "${verdict}" "${name}" "${detail}")")
    printf '%-4s  %-58s %s\n' "${verdict}" "${name}" "${detail}"
}

# client_case NAME EXPECT PROTO CIPHER "S_SERVER ARGS" "PROBE ARGS" [CHECK]
#   EXPECT: ok | fail | posterr (handshake ok, then an I/O error — e.g. a
#   refused renegotiation). PROTO/CIPHER: expected "TLSv1.x"/cipher name, or
#   "-" to skip. CHECK: optional extra: alpn=<p> | bytes>=<n> | grep=<text>.
# s_server always gets -www unless its args carry -WWW; cert/key come from
# the args.
client_case() {
    local name=$1 expect=$2 proto=$3 cipher=$4 sargs=$5 pargs=$6 check=${7:-}
    selected "${name}" || return 0
    local port slog out err mode="-www"
    port="$(free_port)"
    slog="${WORK}/case.srv.log"; out="${WORK}/case.out"; err="${WORK}/case.err"
    [[ "${sargs}" == *"-WWW"* ]] && mode=""
    # shellcheck disable=SC2086
    timeout 20 openssl s_server -accept "${port}" -naccept 1 ${mode} ${sargs} </dev/null >"${slog}" 2>&1 &
    local spid=$!
    if ! wait_listen "${port}"; then
        record "${name}" FAIL "s_server did not start: $(tail -n 3 "${slog}" | tr '\n' ' ')"
        kill "${spid}" 2>/dev/null; wait "${spid}" 2>/dev/null
        return 0
    fi
    # shellcheck disable=SC2086
    timeout 20 "${PROBE}" --connect "127.0.0.1:${port}" ${pargs} >"${out}" 2>"${err}"
    local rc=$?
    kill "${spid}" 2>/dev/null; wait "${spid}" 2>/dev/null
    local probe_line
    probe_line="$(grep '^PROBE' "${err}" | tail -n 1)"
    [[ -z "${probe_line}" ]] && probe_line="(no PROBE line, rc=${rc}) $(tail -n 2 "${err}" | tr '\n' ' ')"
    local got_proto got_cipher
    # The session's "Protocol  :" line is the negotiated version; the "New,"
    # line prints the CIPHER's minimum version (SSL_CIPHER_get_version:
    # TLSv1.0 for the SHA-1 CBC suites even over a 1.2 session).
    got_proto="$(sed -n 's/^ *Protocol *: \(TLSv1\.[0-9]\)$/\1/p' "${out}" | head -n 1)"
    [[ -n "${got_proto}" ]] || got_proto="$(sed -n 's/^New, \(TLSv1\.[0-9]\), Cipher is \(.*\)$/\1/p' "${out}" | head -n 1)"
    got_cipher="$(sed -n 's/^New, \(TLSv1\.[0-9]\), Cipher is \(.*\)$/\2/p' "${out}" | head -n 1)"

    local verdict=PASS why=""
    case "${expect}" in
        ok)
            [[ ${rc} -eq 0 ]] || { verdict=FAIL; why="expected ok, rc=${rc}"; }
            if [[ ${verdict} == PASS && "${proto}" != "-" && "${got_proto}" != "${proto}" ]]; then
                verdict=FAIL; why="protocol ${got_proto:-?} != ${proto}"
            fi
            if [[ ${verdict} == PASS && "${cipher}" != "-" && "${got_cipher}" != "${cipher}" ]]; then
                verdict=FAIL; why="cipher ${got_cipher:-?} != ${cipher}"
            fi
            ;;
        fail) [[ ${rc} -eq 2 ]] || { verdict=FAIL; why="expected handshake failure, rc=${rc}"; } ;;
        posterr) [[ ${rc} -eq 3 ]] || { verdict=FAIL; why="expected post-handshake error, rc=${rc}"; } ;;
    esac
    if [[ ${verdict} == PASS && -n "${check}" ]]; then
        case "${check}" in
            alpn=*) [[ "${probe_line}" == *"alpn=${check#alpn=} "* ]] || { verdict=FAIL; why="want ${check}"; } ;;
            bytes\>=*)
                local want=${check#bytes>=} got
                got="$(sed -n 's/.* bytes=\([0-9]*\).*/\1/p' <<<"${probe_line}")"
                [[ -n "${got}" && ${got} -ge ${want} ]] || { verdict=FAIL; why="bytes=${got:-?} < ${want}"; } ;;
            grep=*) grep -q -- "${check#grep=}" "${out}" || { verdict=FAIL; why="response lacks '${check#grep=}'"; } ;;
        esac
    fi
    local srv_note
    srv_note="$(grep -m1 -iE 'error|alert|no shared|wrong' "${slog}" | head -c 110)"
    local detail="${probe_line#PROBE }"
    [[ -n "${got_proto}" ]] && detail="${got_proto} ${got_cipher} | ${detail}"
    [[ ${expect} != ok && -n "${srv_note}" ]] && detail="${detail} | openssl: ${srv_note}"
    [[ -n "${why}" ]] && detail="${why} :: ${detail}"
    record "${name}" "${verdict}" "${detail}"
    if [[ ${VERBOSE} -eq 1 && ${verdict} == FAIL ]]; then
        echo "---- probe stderr"; cat "${err}"; echo "---- s_server log"; tail -n 20 "${slog}"; echo "----"
    fi
}

# Scripted (non -www) s_server: stdin commands drive post-handshake events.
# script_case NAME EXPECT "S_SERVER ARGS" "STDIN SCRIPT (bash)" "PROBE ARGS" [CHECK...]
#   EXPECT: data (handshake ok, checks hold; the close is not judged — the
#   scripted `q` quits without close_notify) | posterr.
#   CHECK: grep=<text in the response> | srvgrep=<text the server received>.
script_case() {
    local name=$1 expect=$2 sargs=$3 feed=$4 pargs=$5
    shift 5
    selected "${name}" || return 0
    local port slog out err
    port="$(free_port)"
    slog="${WORK}/case.srv.log"; out="${WORK}/case.out"; err="${WORK}/case.err"
    # shellcheck disable=SC2086
    (eval "${feed}") | timeout 20 openssl s_server -accept "${port}" -naccept 1 ${sargs} >"${slog}" 2>&1 &
    local spid=$!
    if ! wait_listen "${port}"; then
        record "${name}" FAIL "s_server did not start"
        kill "${spid}" 2>/dev/null; wait "${spid}" 2>/dev/null
        return 0
    fi
    # shellcheck disable=SC2086
    timeout 20 "${PROBE}" --connect "127.0.0.1:${port}" ${pargs} >"${out}" 2>"${err}"
    local rc=$?
    wait "${spid}" 2>/dev/null
    local probe_line verdict=PASS why=""
    probe_line="$(grep '^PROBE' "${err}" | tail -n 1)"
    case "${expect}" in
        data) [[ "${probe_line}" == *"handshake=ok"* ]] || { verdict=FAIL; why="expected handshake ok, rc=${rc}"; } ;;
        posterr) [[ ${rc} -eq 3 ]] || { verdict=FAIL; why="expected post-handshake error, rc=${rc}"; } ;;
    esac
    local check
    for check in "$@"; do
        [[ ${verdict} == PASS ]] || break
        case "${check}" in
            grep=*) grep -q -- "${check#grep=}" "${out}" || { verdict=FAIL; why="response lacks '${check#grep=}'"; } ;;
            srvgrep=*) grep -q -- "${check#srvgrep=}" "${slog}" || { verdict=FAIL; why="server never got '${check#srvgrep=}'"; } ;;
        esac
    done
    local detail="${probe_line#PROBE }"
    [[ -n "${why}" ]] && detail="${why} :: ${detail}"
    record "${name}" "${verdict}" "${detail}"
    if [[ ${VERBOSE} -eq 1 && ${verdict} == FAIL ]]; then
        echo "---- probe stderr"; cat "${err}"; echo "---- s_server log"; tail -n 20 "${slog}"; echo "----"
    fi
}

# server_case NAME EXPECT PROTO CIPHER "PROBE --listen ARGS" "S_CLIENT ARGS" [CHECK]
server_case() {
    local name=$1 expect=$2 proto=$3 cipher=$4 pargs=$5 cargs=$6 check=${7:-}
    selected "${name}" || return 0
    local port plog out
    port="$(free_port)"
    plog="${WORK}/case.probe.log"; out="${WORK}/case.cli.out"
    # shellcheck disable=SC2086
    timeout 20 "${PROBE}" --listen "${port}" --accept 1 ${pargs} >/dev/null 2>"${plog}" &
    local ppid=$!
    if ! wait_listen "${port}"; then
        record "${name}" FAIL "tls_probe --listen did not start: $(tail -n 2 "${plog}" | tr '\n' ' ')"
        kill "${ppid}" 2>/dev/null; wait "${ppid}" 2>/dev/null
        return 0
    fi
    # shellcheck disable=SC2086
    printf 'hello-from-openssl\n' | timeout 20 openssl s_client -connect "127.0.0.1:${port}" -ign_eof \
        ${cargs} >"${out}" 2>&1
    local rc=$?
    kill "${ppid}" 2>/dev/null; wait "${ppid}" 2>/dev/null
    local serve_line got_proto got_cipher verdict=PASS why=""
    serve_line="$(grep '^SERVE handshake' "${plog}" | tail -n 1)"
    got_proto="$(sed -n 's/^New, \(TLSv1\.[0-9]\), Cipher is \(.*\)$/\1/p' "${out}" | head -n 1)"
    got_cipher="$(sed -n 's/^New, \(TLSv1\.[0-9]\), Cipher is \(.*\)$/\2/p' "${out}" | head -n 1)"
    case "${expect}" in
        ok)
            if [[ "${serve_line}" != *"handshake=ok"* ]] || ! grep -q 'PROBE-ECHO hello-from-openssl' "${out}"; then
                verdict=FAIL; why="expected ok (s_client rc=${rc})"
            fi
            if [[ ${verdict} == PASS && "${proto}" != "-" && "${got_proto}" != "${proto}" ]]; then
                verdict=FAIL; why="protocol ${got_proto:-?} != ${proto}"
            fi
            if [[ ${verdict} == PASS && "${cipher}" != "-" && "${got_cipher}" != "${cipher}" ]]; then
                verdict=FAIL; why="cipher ${got_cipher:-?} != ${cipher}"
            fi
            ;;
        fail) [[ "${serve_line}" != *"handshake=ok"* ]] || { verdict=FAIL; why="expected handshake failure"; } ;;
    esac
    if [[ ${verdict} == PASS && "${check}" == alpn=* ]]; then
        [[ "${serve_line}" == *"alpn=${check#alpn=} "* ]] || { verdict=FAIL; why="want ${check}"; }
    fi
    local cli_note
    cli_note="$(grep -m1 -iE 'alert|error:' "${out}" | head -c 110)"
    local detail="${serve_line#SERVE }"
    [[ -z "${serve_line}" ]] && detail="(no SERVE line) $(tail -n 1 "${plog}")"
    [[ -n "${got_proto}" ]] && detail="${got_proto} ${got_cipher} | ${detail}"
    [[ ${expect} != ok && -n "${cli_note}" ]] && detail="${detail} | openssl: ${cli_note}"
    [[ -n "${why}" ]] && detail="${why} :: ${detail}"
    record "${name}" "${verdict}" "${detail}"
    if [[ ${VERBOSE} -eq 1 && ${verdict} == FAIL ]]; then
        echo "---- probe log"; cat "${plog}"; echo "---- s_client out"; tail -n 30 "${out}"; echo "----"
    fi
}

RSA="-cert rsa2048.pem -key rsa2048.key -cert_chain int.pem"
P256="-cert p256.pem -key p256.key -cert_chain int.pem"
CA="--ca root.pem --sni localhost"

# ================================================================ part 1
echo
echo "== Part 1: our client -> openssl s_server"

# -- protocol x cipher suite
for cs in TLS_AES_128_GCM_SHA256 TLS_AES_256_GCM_SHA384 TLS_CHACHA20_POLY1305_SHA256; do
    client_case "c/1.3 ${cs}" ok TLSv1.3 "${cs}" "${RSA} -tls1_3 -ciphersuites ${cs}" "${CA}"
done
for c in ECDHE-RSA-AES128-GCM-SHA256 ECDHE-RSA-AES256-GCM-SHA384 ECDHE-RSA-CHACHA20-POLY1305; do
    client_case "c/1.2 ${c}" ok TLSv1.2 "${c}" "${RSA} -tls1_2 -cipher ${c}" "${CA}"
done
for c in ECDHE-ECDSA-AES128-GCM-SHA256 ECDHE-ECDSA-AES256-GCM-SHA384 ECDHE-ECDSA-CHACHA20-POLY1305; do
    client_case "c/1.2 ${c}" ok TLSv1.2 "${c}" "${P256} -tls1_2 -cipher ${c}" "${CA}"
done
# Legacy 1.2 suites (feature/tls/11): the client offers them after the whole
# AEAD order, for servers that speak nothing newer.
for c in ECDHE-RSA-AES128-SHA ECDHE-RSA-AES256-SHA ECDHE-RSA-AES128-SHA256; do
    client_case "c/1.2 legacy ${c}" ok TLSv1.2 "${c}" "${RSA} -tls1_2 -cipher ${c}" "${CA}"
done
for c in ECDHE-ECDSA-AES128-SHA ECDHE-ECDSA-AES256-SHA; do
    client_case "c/1.2 legacy ${c}" ok TLSv1.2 "${c}" "${P256} -tls1_2 -cipher ${c}" "${CA}"
done
client_case "c/1.2 legacy tail loses to AEAD" ok TLSv1.2 ECDHE-RSA-AES128-GCM-SHA256 "${RSA} -tls1_2 -cipher ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES128-GCM-SHA256" "${CA}"
client_case "c/1.2 non-ECDHE only (AES128-GCM-SHA256)" fail - - "${RSA} -tls1_2 -cipher AES128-GCM-SHA256" "${CA}"

# -- key exchange groups
client_case "c/1.3 group X25519" ok TLSv1.3 - "${RSA} -tls1_3 -groups X25519" "${CA}"
client_case "c/1.3 group P-256" ok TLSv1.3 - "${RSA} -tls1_3 -groups P-256" "${CA}"
client_case "c/1.3 group P-256 (server prefers, HRR if needed)" ok TLSv1.3 - "${RSA} -tls1_3 -groups P-256:X25519 -serverpref" "${CA}"
client_case "c/1.3 group P-384 only (HRR to P-384)" ok TLSv1.3 - "${RSA} -tls1_3 -groups P-384" "${CA}"
client_case "c/1.3 group ffdhe2048 only (unsupported)" fail - - "${RSA} -tls1_3 -groups ffdhe2048" "${CA}"
client_case "c/1.2 group X25519" ok TLSv1.2 - "${RSA} -tls1_2 -groups X25519" "${CA}"
client_case "c/1.2 group P-256" ok TLSv1.2 - "${RSA} -tls1_2 -groups P-256" "${CA}"
client_case "c/1.2 group P-384" ok TLSv1.2 - "${RSA} -tls1_2 -groups P-384" "${CA}"

# -- server key / signature algorithm
client_case "c/1.3 RSA-2048 rsa_pss_rsae_sha256" ok TLSv1.3 - "${RSA} -tls1_3 -sigalgs rsa_pss_rsae_sha256" "${CA}"
client_case "c/1.3 RSA-2048 rsa_pss_rsae_sha384" ok TLSv1.3 - "${RSA} -tls1_3 -sigalgs rsa_pss_rsae_sha384" "${CA}"
client_case "c/1.3 RSA-2048 rsa_pss_rsae_sha512" ok TLSv1.3 - "${RSA} -tls1_3 -sigalgs rsa_pss_rsae_sha512" "${CA}"
client_case "c/1.3 RSA-4096" ok TLSv1.3 - "-cert rsa4096.pem -key rsa4096.key -cert_chain int.pem -tls1_3" "${CA}"
client_case "c/1.3 ECDSA P-256" ok TLSv1.3 - "${P256} -tls1_3" "${CA}"
client_case "c/1.3 ECDSA P-384" ok TLSv1.3 - "-cert p384.pem -key p384.key -cert_chain int.pem -tls1_3" "${CA}"
client_case "c/1.3 Ed25519" ok TLSv1.3 - "-cert ed25519.pem -key ed25519.key -cert_chain int.pem -tls1_3" "${CA}"
client_case "c/1.3 RSA-PSS key (rsa_pss_pss_*, not offered)" fail - - "-cert rsapss.pem -key rsapss.key -cert_chain int.pem -tls1_3" "${CA}"
client_case "c/1.2 RSA rsa_pkcs1_sha256" ok TLSv1.2 - "${RSA} -tls1_2 -sigalgs RSA+SHA256" "${CA}"
client_case "c/1.2 RSA rsa_pkcs1_sha384" ok TLSv1.2 - "${RSA} -tls1_2 -sigalgs RSA+SHA384" "${CA}"
client_case "c/1.2 RSA rsa_pss_rsae_sha256" ok TLSv1.2 - "${RSA} -tls1_2 -sigalgs rsa_pss_rsae_sha256" "${CA}"
client_case "c/1.2 ECDSA P-256" ok TLSv1.2 - "${P256} -tls1_2" "${CA}"
# In 1.2 the client's supported_groups also bounds the server certificate's
# curve (RFC 8422 §5.1): this needs P-384 in our supported_groups.
client_case "c/1.2 ECDSA P-384" ok TLSv1.2 - "-cert p384.pem -key p384.key -cert_chain int.pem -tls1_2" "${CA}"
client_case "c/1.2 Ed25519" ok TLSv1.2 - "-cert ed25519.pem -key ed25519.key -cert_chain int.pem -tls1_2" "${CA}"

# -- certificate verification
client_case "c/verify hostname via SNI" ok - - "${RSA}" "--ca root.pem --sni interop.test"
client_case "c/verify IP SAN (verify-name 127.0.0.1)" ok - - "${RSA}" "--ca root.pem --verify-name 127.0.0.1"
client_case "c/verify wrong hostname" fail - - "${RSA}" "--ca root.pem --sni wrong.test"
client_case "c/verify untrusted root" fail - - "${RSA}" "--ca other.pem --sni localhost"
client_case "c/verify missing intermediate" fail - - "-cert rsa2048.pem -key rsa2048.key" "${CA}"
client_case "c/verify expired leaf" fail - - "-cert expired.pem -key rsa2048.key -cert_chain int.pem" "${CA}"
client_case "c/verify no verification (no --ca)" ok - - "-cert rsa2048.pem -key rsa2048.key" "--sni localhost"
client_case "c/chain two intermediates" ok - - "-cert long.pem -key rsa2048.key -cert_chain long.extra.pem" "${CA}"
client_case "c/chain server also sends root" ok - - "-cert rsa2048.pem -key rsa2048.key -cert_chain withroot.extra.pem" "${CA}"
client_case "c/chain 6 entries" ok - - "-cert rsa2048.pem -key rsa2048.key -cert_chain six.extra.pem" "${CA}"

# -- ALPN
client_case "c/alpn h2 selected" ok - - "${RSA} -alpn h2,http/1.1" "${CA} --alpn h2,http/1.1" "alpn=h2"
client_case "c/alpn http/1.1 selected" ok - - "${RSA} -alpn http/1.1" "${CA} --alpn h2,http/1.1" "alpn=http/1.1"
client_case "c/alpn server without ALPN" ok - - "${RSA}" "${CA} --alpn h2,http/1.1" "alpn=-"
client_case "c/1.2 alpn h2 selected" ok TLSv1.2 - "${RSA} -tls1_2 -alpn h2" "${CA} --alpn h2,http/1.1" "alpn=h2"

# -- client certificates (mTLS)
client_case "c/1.3 mTLS required, cert sent" ok TLSv1.3 - "${RSA} -tls1_3 -Verify 2 -CAfile root.pem" "${CA} --cert client.chain.pem --key client.key" "grep=Client certificate"
# 1.3: the client's handshake completes before the server judges its (empty)
# Certificate, so the rejection legitimately arrives post-handshake.
client_case "c/1.3 mTLS required, no cert" posterr - - "${RSA} -tls1_3 -Verify 2 -CAfile root.pem -verify_return_error" "${CA}"
client_case "c/1.3 mTLS optional, no cert" ok TLSv1.3 - "${RSA} -tls1_3 -verify 2 -CAfile root.pem" "${CA}"
client_case "c/1.2 mTLS required, cert sent" ok TLSv1.2 - "${RSA} -tls1_2 -Verify 2 -CAfile root.pem" "${CA} --cert client.chain.pem --key client.key" "grep=Client certificate"
client_case "c/1.2 mTLS required, no cert" fail - - "${RSA} -tls1_2 -Verify 2 -CAfile root.pem -verify_return_error" "${CA}"

# -- version negotiation
client_case "c/version default -> 1.3" ok TLSv1.3 - "${RSA}" "${CA}"
client_case "c/version client max 1.2" ok TLSv1.2 - "${RSA}" "${CA} --max 1.2"
client_case "c/version server 1.2 only" ok TLSv1.2 - "${RSA} -tls1_2" "${CA}"
client_case "c/version client min 1.3 vs server 1.2" fail - - "${RSA} -tls1_2" "${CA} --min 1.3"
client_case "c/version client max 1.2 vs server 1.3" fail - - "${RSA} -tls1_3" "${CA} --max 1.2"
client_case "c/version server 1.1 only" fail - - "${RSA} -tls1_1 -cipher DEFAULT:@SECLEVEL=0" "${CA}"

# -- records, tickets, middlebox compat
client_case "c/1.3 1 MiB response" ok - - "${RSA} -tls1_3 -WWW" "${CA} --get /big.bin" "bytes>=1048576"
client_case "c/1.2 1 MiB response" ok - - "${RSA} -tls1_2 -WWW" "${CA} --get /big.bin" "bytes>=1048576"
client_case "c/1.3 1 MiB, 512-byte records" ok - - "${RSA} -tls1_3 -WWW -max_send_frag 512" "${CA} --get /big.bin" "bytes>=1048576"
client_case "c/1.2 1 MiB, 512-byte records" ok - - "${RSA} -tls1_2 -WWW -max_send_frag 512" "${CA} --get /big.bin" "bytes>=1048576"
client_case "c/1.2 1 MiB, ECDHE-RSA-AES128-SHA" ok - - "${RSA} -tls1_2 -cipher ECDHE-RSA-AES128-SHA -WWW" "${CA} --get /big.bin" "bytes>=1048576"
client_case "c/1.2 1 MiB, 512-byte records, ECDHE-RSA-AES128-SHA256" ok - - "${RSA} -tls1_2 -cipher ECDHE-RSA-AES128-SHA256 -WWW -max_send_frag 512" "${CA} --get /big.bin" "bytes>=1048576"
client_case "c/1.3 8 session tickets" ok TLSv1.3 - "${RSA} -tls1_3 -num_tickets 8" "${CA}"
client_case "c/1.3 no session tickets" ok TLSv1.3 - "${RSA} -tls1_3 -num_tickets 0" "${CA}"
client_case "c/1.3 no middlebox compat" ok TLSv1.3 - "${RSA} -tls1_3 -no_middlebox" "${CA}"
client_case "c/1.2 no session ticket" ok TLSv1.2 - "${RSA} -tls1_2 -no_ticket" "${CA}"

# -- post-handshake events (scripted s_server: K = KeyUpdate requested,
#    R = renegotiation request, q = close the connection)
# Requested: after reading "after-ku" the probe writes again — our KeyUpdate
# response must precede that data under the rotated keys, or OpenSSL cannot
# decrypt it (srvgrep proves it arrived intact).
script_case "c/1.3 server KeyUpdate(update_requested) + our response" data "${RSA} -tls1_3" \
    "sleep 1; printf 'before-ku\n'; sleep 0.3; printf 'K\n'; sleep 0.3; printf 'after-ku\n'; sleep 1; printf 'q\n'; sleep 1" \
    "${CA} --request hello\\n --then-after after-ku --then post-ku-write\\n" "grep=after-ku" "srvgrep=post-ku-write"
script_case "c/1.3 server KeyUpdate(not requested)" data "${RSA} -tls1_3" \
    "sleep 1; printf 'k\n'; sleep 0.3; printf 'after-ku\n'; sleep 1; printf 'q\n'; sleep 1" \
    "${CA} --request hello\\n --then-after after-ku --then post-ku-write\\n" "grep=after-ku" "srvgrep=post-ku-write"
script_case "c/1.2 server renegotiation request refused" posterr "${RSA} -tls1_2" \
    "sleep 1; printf 'R\n'; sleep 1.5; printf 'q\n'; sleep 1" \
    "${CA} --request hello\\n"

# ================================================================ part 2
echo
echo "== Part 2: openssl s_client -> our server"

SRV_RSA="--cert rsa2048.chain.pem --key rsa2048.key"
SRV_P256="--cert p256.chain.pem --key p256.key"
CLI="-servername localhost -CAfile root.pem -verify_return_error"

for cs in TLS_AES_128_GCM_SHA256 TLS_AES_256_GCM_SHA384 TLS_CHACHA20_POLY1305_SHA256; do
    server_case "s/1.3 ${cs}" ok TLSv1.3 "${cs}" "${SRV_RSA}" "${CLI} -tls1_3 -ciphersuites ${cs}"
done
for c in ECDHE-RSA-AES128-GCM-SHA256 ECDHE-RSA-AES256-GCM-SHA384 ECDHE-RSA-CHACHA20-POLY1305; do
    server_case "s/1.2 ${c}" ok TLSv1.2 "${c}" "${SRV_RSA}" "${CLI} -tls1_2 -cipher ${c}"
done
for c in ECDHE-ECDSA-AES128-GCM-SHA256 ECDHE-ECDSA-AES256-GCM-SHA384 ECDHE-ECDSA-CHACHA20-POLY1305; do
    server_case "s/1.2 ${c}" ok TLSv1.2 "${c}" "${SRV_P256}" "${CLI} -tls1_2 -cipher ${c}"
done
server_case "s/1.2 CBC only offered" fail - - "${SRV_RSA}" "${CLI} -tls1_2 -cipher ECDHE-RSA-AES128-SHA256"
server_case "s/1.3 group X25519" ok TLSv1.3 - "${SRV_RSA}" "${CLI} -tls1_3 -groups X25519"
server_case "s/1.3 group P-256" ok TLSv1.3 - "${SRV_RSA}" "${CLI} -tls1_3 -groups P-256"
server_case "s/1.3 HRR (shares P-384, also offers P-256)" ok TLSv1.3 - "${SRV_RSA}" "${CLI} -tls1_3 -groups P-384:P-256"
server_case "s/1.3 group P-384 only" ok TLSv1.3 - "${SRV_RSA}" "${CLI} -tls1_3 -groups P-384"
server_case "s/1.3 group ffdhe2048 only (unsupported)" fail - - "${SRV_RSA}" "${CLI} -tls1_3 -groups ffdhe2048"
server_case "s/1.3 ECDSA P-256" ok TLSv1.3 - "${SRV_P256}" "${CLI} -tls1_3"
server_case "s/1.3 Ed25519" ok TLSv1.3 - "--cert ed25519.chain.pem --key ed25519.key" "${CLI} -tls1_3"
server_case "s/1.3 RSA sigalg rsa_pss_rsae_sha384" ok TLSv1.3 - "${SRV_RSA}" "${CLI} -tls1_3 -sigalgs rsa_pss_rsae_sha384"
server_case "s/1.2 RSA sigalg rsa_pkcs1_sha256" ok TLSv1.2 - "${SRV_RSA}" "${CLI} -tls1_2 -sigalgs RSA+SHA256"
server_case "s/1.2 group P-256" ok TLSv1.2 - "${SRV_RSA}" "${CLI} -tls1_2 -groups P-256"
server_case "s/1.2 group P-384" ok TLSv1.2 - "${SRV_RSA}" "${CLI} -tls1_2 -groups P-384"
server_case "s/1.3 ECDSA P-384 certificate" ok TLSv1.3 - "--cert p384.chain.pem --key p384.key" "${CLI} -tls1_3"
server_case "s/1.2 ECDSA P-384 certificate" ok TLSv1.2 ECDHE-ECDSA-AES128-GCM-SHA256 "--cert p384.chain.pem --key p384.key" "${CLI} -tls1_2 -cipher ECDHE-ECDSA-AES128-GCM-SHA256"
server_case "s/alpn h2" ok - - "${SRV_RSA} --alpn h2,http/1.1" "${CLI} -alpn h2,http/1.1" "alpn=h2"
server_case "s/alpn http/1.1 only" ok - - "${SRV_RSA} --alpn h2,http/1.1" "${CLI} -alpn http/1.1" "alpn=http/1.1"
server_case "s/1.3 mTLS required, cert sent" ok TLSv1.3 - "${SRV_RSA} --ca root.pem --require-client-cert 1" "${CLI} -tls1_3 -cert client.pem -key client.key -cert_chain int.pem"
server_case "s/1.3 mTLS required, no cert" fail - - "${SRV_RSA} --ca root.pem --require-client-cert 1" "${CLI} -tls1_3"
server_case "s/1.2 mTLS required, cert sent" ok TLSv1.2 - "${SRV_RSA} --ca root.pem --require-client-cert 1" "${CLI} -tls1_2 -cert client.pem -key client.key -cert_chain int.pem"
server_case "s/1.3 no middlebox compat" ok TLSv1.3 - "${SRV_RSA}" "${CLI} -tls1_3 -no_middlebox"
server_case "s/version client 1.1 only" fail - - "${SRV_RSA}" "${CLI} -tls1_1 -cipher DEFAULT:@SECLEVEL=0"
server_case "s/version server min 1.3, client 1.2" fail - - "${SRV_RSA} --min 1.3" "${CLI} -tls1_2"

# ================================================================ summary
echo
echo "== Summary: ${PASS} passed, ${FAIL} failed ($((PASS + FAIL)) cases)"
if [[ ${FAIL} -gt 0 ]]; then
    printf '%s\n' "${RESULTS[@]}" | grep '^FAIL'
    exit 1
fi
exit 0
