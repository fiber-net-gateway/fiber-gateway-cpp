#!/usr/bin/env bash
# H3 multi-sample matrix #6 (tls branch re-run). Usage: run_h3_6.sh <samples>
# Writes results6/s*/; waits for UDP/TCP port release between rounds (#5 race fix).
set -u
cd /software/Code/fiber-gateway-cpp
SAMPLES=$1
for s in $(seq 1 "$SAMPLES"); do
  for p in lite nginx; do
    echo ">>> sample $s h3 $p $(date +%T)"
    RES_DIR=/software/Code/fiber-gateway-cpp/temp/bench/results6/s$s \
      bash temp/bench/bench3.sh h3 "$p" >/dev/null 2>&1
    echo "--- exit $? $(date +%T)"
    for _ in $(seq 1 40); do
      ss -tuln 2>/dev/null | grep -qE ':(18443|38443)\b' || break
      sleep 0.5
    done
  done
done
echo "matrix h3 done"
