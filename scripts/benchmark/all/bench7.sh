#!/usr/bin/env bash
# Usage: bench7.sh <protocol> <proxy>   (#7 = bench3.sh + per-scenario H3 worker-tier measurement)
#   protocol: h1 | h2 | h3
#   proxy:    lite | openresty | nginx
# CPU layout: backend 0-3, proxy 4-9, loadgen 10-15
# Tier (#7): for h3 runs, worker hit count is MEASURED per scenario via
# utime+stime deltas around the load (all_benchmark_6 §7.5 errata-of-errata:
# guessing tiers produced a false second-factor conclusion). Appends
# "TIER_WORKERS: n" to the scenario .txt. lite: worker threads of the single
# process (main thread excluded — it is not part of the reuseport lottery);
# nginx: worker processes under the master.
set -u
cd /software/Code/fiber-gateway-cpp

ROOT=/software/Code/fiber-gateway-cpp
WRK=$ROOT/temp/wrk-4.2.0/wrk
H2LOAD=$ROOT/temp/nghttp2-1.65.0/build/src/h2load
H3C=$ROOT/build/example/http3_benchmark_client
LITE=$ROOT/build/apps/lite_nginx
OR=$ROOT/temp/openresty-install/nginx/sbin/nginx
NG=$ROOT/temp/nginx-install/sbin/nginx
CONF=$ROOT/temp/bench
RES=${RES_DIR:-$ROOT/temp/bench/results}
CERT=$CONF/cert.pem
BODY=$CONF/post_1m.bin
LUA=$CONF/post_1m.lua
mkdir -p "$RES"

PROTO=$1
PROXY=$2
DURATION=20s
WARMUP=3s
# load params per protocol
WRK_FLAGS=(-t6 -c256 -d$DURATION --latency)
H2_FLAGS=(-t6 -c32 -m16 -D$DURATION)
H3_FLAGS=(--threads 4 --connections 16 --streams 4 --duration $DURATION --warmup $WARMUP --insecure)

case $PROXY in
  lite)      H1P=18081; TLSP=18443
             START() { taskset -c 4-9 "$LITE" --config "$CONF/lite_nginx.conf" >"$RES/lite.log" 2>&1 & echo $! >"$RES/lite.pid"; }
             STOP()  { kill "$(cat "$RES/lite.pid")" 2>/dev/null; sleep 1; } ;;
  openresty) H1P=28080; TLSP=28443
             START() { taskset -c 4-9 "$OR" -p "$CONF/openresty_run" -c "$CONF/openresty.conf" 2>>"$RES/openresty.log"; }
             STOP()  { "$OR" -p "$CONF/openresty_run" -c "$CONF/openresty.conf" -s stop 2>/dev/null; sleep 1; } ;;
  nginx)     H1P=38080; TLSP=38443
             # bind pre-probe: 38080/38443 sit in the ephemeral port range; an
             # outbound source port held in ESTABLISHED/TIME_WAIT (invisible to
             # ss -tuln) blocks nginx's bind (#6 §5.5 race, recurred in #7 s3 h1).
             # Probe mirrors nginx exactly: SO_REUSEADDR + 0.0.0.0, TCP and UDP.
             START() {
               for _ in $(seq 1 40); do
                 if python3 - "$H1P" "$TLSP" <<'EOF'
import socket, sys
for port in (int(sys.argv[1]), int(sys.argv[2])):
    for typ in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        s = socket.socket(socket.AF_INET, typ)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("0.0.0.0", port))
        except OSError:
            sys.exit(1)
        finally:
            s.close()
EOF
                 then break; fi
                 sleep 0.5
               done
               taskset -c 4-9 "$NG" -p "$CONF/nginx_run" -c "$CONF/nginx_quic.conf" 2>>"$RES/nginx.log"; }
             STOP()  { "$NG" -p "$CONF/nginx_run" -c "$CONF/nginx_quic.conf" -s stop 2>/dev/null; sleep 1; } ;;
  *) echo "unknown proxy $PROXY"; exit 1 ;;
esac

# scenario definitions: name path method expect_bytes
SCEN=(
  "get1k   /bench/1k    GET  1024"
  "get64k  /bench/64k   GET  65536"
  "get1m   /bench/1m    GET  1048576"
  "post1m  /bench/echo  POST 1048576"
)

# --- worker-tier measurement (h3 only) ---
nginx_bench_master() { # bench nginx master pid via /proc walk; no pid file is
  # written (nginx_run/logs/ absent) and pgrep -f self-matches this script's
  # own cmdline. Master cmdline carries the invocation (workers' is just
  # "nginx: worker process"), so comm=nginx + nginx_run + nginx_quic.conf
  # uniquely identifies OUR master; the resident cat-demo nginx uses a
  # different prefix/conf. Masters are NOT reparented to init on this host
  # (PPid=4346 subreaper) — no PPid filter. If a draining previous master
  # lingers, take the highest pid (most recent round).
  local d best=0
  for d in /proc/[0-9]*; do
    [ "$(cat "$d/comm" 2>/dev/null)" = nginx ] || continue
    grep -qa 'nginx_run' "$d/cmdline" 2>/dev/null || continue
    grep -qa 'nginx_quic.conf' "$d/cmdline" 2>/dev/null || continue
    d=${d#/proc/}
    [ "$d" -gt "$best" ] && best=$d
  done
  [ "$best" -gt 0 ] || return 1
  echo "$best"
}
cpu_snap() { # emit "id utime+stime" for candidate serving units
  if [ "$PROXY" = lite ]; then
    local p; p=$(cat "$RES/lite.pid" 2>/dev/null)
    [ -n "$p" ] && awk -v main="$p" '$1 != main {print $1, $14+$15}' /proc/$p/task/*/stat 2>/dev/null
  elif [ "$PROXY" = nginx ]; then
    local m; m=$(nginx_bench_master)
    [ -n "$m" ] && for ch in $(pgrep -P "$m" 2>/dev/null); do
      awk '{print $1, $14+$15}' /proc/$ch/stat 2>/dev/null
    done
  fi
}
# >=300 jiffies (3s CPU at 100/s per core) = served; noise threads stay well below
tier_count() { awk 'NR==FNR{b[$1]=$2;next}{if($2-b[$1]>=300)n++}END{print n+0}' "$1" "$2"; }

run_one() {
  local scen=$1 path=$2 method=$3 expect=$4
  local out="$RES/${PROTO}_${PROXY}_${scen}.txt"
  local url
  echo "=== [$PROTO/$PROXY] $scen ($method $path) ===" | tee "$out"
  local snapA="$RES/.tier_a" snapB="$RES/.tier_b"
  [ "$PROTO" = h3 ] && cpu_snap >"$snapA"
  case $PROTO in
    h1)
      if [ "$method" = POST ]; then
        taskset -c 10-15 "$WRK" "${WRK_FLAGS[@]}" -s "$LUA" "http://127.0.0.1:$H1P" >>"$out" 2>&1
      else
        taskset -c 10-15 "$WRK" "${WRK_FLAGS[@]}" "http://127.0.0.1:$H1P$path" >>"$out" 2>&1
      fi ;;
    h2)
      if [ "$method" = POST ]; then
        SSL_CERT_FILE="$CERT" taskset -c 10-15 "$H2LOAD" "${H2_FLAGS[@]}" --data="$BODY" "https://127.0.0.1:$TLSP$path" >>"$out" 2>&1
      else
        SSL_CERT_FILE="$CERT" taskset -c 10-15 "$H2LOAD" "${H2_FLAGS[@]}" "https://127.0.0.1:$TLSP$path" >>"$out" 2>&1
      fi ;;
    h3)
      if [ "$method" = POST ]; then
        taskset -c 10-15 "$H3C" "${H3_FLAGS[@]}" --method POST --body "$BODY" --expect-bytes "$expect" --json "${out%.txt}.json" "https://127.0.0.1:$TLSP$path" >>"$out" 2>&1
      else
        taskset -c 10-15 "$H3C" "${H3_FLAGS[@]}" --expect-bytes "$expect" --json "${out%.txt}.json" "https://127.0.0.1:$TLSP$path" >>"$out" 2>&1
      fi
      cpu_snap >"$snapB"
      echo "TIER_WORKERS: $(tier_count "$snapA" "$snapB")" >>"$out" ;;
  esac
  tail -3 "$out"
}

# health check backend
curl -s -o /dev/null "http://127.0.0.1:19001/bench/1k" || { echo "backend down!"; exit 1; }

echo "### START $PROTO $PROXY"
START
sleep 2
for s in "${SCEN[@]}"; do
  # shellcheck disable=SC2086
  run_one $s
  sleep 1
done
STOP
echo "### DONE $PROTO $PROXY"
