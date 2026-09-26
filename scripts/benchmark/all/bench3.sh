#!/usr/bin/env bash
# Usage: bench.sh <protocol> <proxy>
#   protocol: h1 | h2 | h3
#   proxy:    lite | openresty | nginx
# CPU layout: backend 0-3, proxy 4-9, loadgen 10-15
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
             START() { taskset -c 4-9 "$NG" -p "$CONF/nginx_run" -c "$CONF/nginx_quic.conf" 2>>"$RES/nginx.log"; }
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

run_one() {
  local scen=$1 path=$2 method=$3 expect=$4
  local out="$RES/${PROTO}_${PROXY}_${scen}.txt"
  local url
  echo "=== [$PROTO/$PROXY] $scen ($method $path) ===" | tee "$out"
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
      fi ;;
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
