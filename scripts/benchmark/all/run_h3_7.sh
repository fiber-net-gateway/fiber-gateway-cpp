#!/usr/bin/env bash
# H3 multi-sample matrix #7 (post-fix verification, tier-measured). Usage: run_h3_7.sh <samples>
# Writes results7/s*/ (bench7.sh appends TIER_WORKERS per scenario); waits for
# UDP/TCP port release between rounds.
set -u
cd /software/Code/fiber-gateway-cpp
SAMPLES=$1
for s in $(seq 1 "$SAMPLES"); do
  for p in lite nginx; do
    echo ">>> sample $s h3 $p $(date +%T)"
    RES_DIR=/software/Code/fiber-gateway-cpp/temp/bench/results7/s$s \
      bash temp/bench/bench7.sh h3 "$p" >/dev/null 2>&1
    echo "--- exit $? $(date +%T)"
    for _ in $(seq 1 40); do
      ss -tuln 2>/dev/null | grep -qE ':(18443|38443)\b' || break
      sleep 0.5
    done
  done
done
echo "matrix h3 done"
