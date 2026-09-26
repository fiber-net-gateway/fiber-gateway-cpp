#!/usr/bin/env bash
# Multi-sample benchmark matrix #7 (post-fix verification). Usage: run_matrix7.sh <proto> <samples>
#   e.g. run_matrix7.sh h1 3   -> results7/s1..s3/{h1_lite_get1k,...}.txt
# Interleaves proxies per sample to spread time-order bias; waits for port
# release between rounds (see #5/#6 race notes). Uses bench7.sh (tier-aware).
set -u
cd /software/Code/fiber-gateway-cpp
PROTO=$1
SAMPLES=$2
case $PROTO in
  h1|h2) PROXIES="lite openresty nginx" ;;
  h3)    PROXIES="lite nginx" ;;  # OpenResty has no H3
  *) echo "unknown proto"; exit 1 ;;
esac

wait_ports_free() {
  for _ in $(seq 1 40); do
    if ! ss -tuln 2>/dev/null | grep -qE ':(18081|18443|28080|28443|38080|38443)\b'; then
      return 0
    fi
    sleep 0.5
  done
  echo "WARN: ports still held after 20s"
}

for s in $(seq 1 "$SAMPLES"); do
  for p in $PROXIES; do
    echo ">>> sample $s $PROTO $p $(date +%T)"
    RES_DIR=/software/Code/fiber-gateway-cpp/temp/bench/results7/s$s \
      bash temp/bench/bench7.sh "$PROTO" "$p" >/dev/null 2>&1
    echo "--- exit $? $(date +%T)"
    wait_ports_free
  done
done
echo "matrix $PROTO done"
