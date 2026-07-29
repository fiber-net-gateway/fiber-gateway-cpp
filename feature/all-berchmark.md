# lite-nginx reverse proxy 全协议本机性能对比

测试日期：2026-07-29（Asia/Shanghai）

## 1. 结论

本报告比较的是同一台机器上的完整反向代理链路：

```text
load client -> lite-nginx 或 OpenResty -> HTTP/1.1 benchmark backend
```

主要结论：

1. **HTTP/1.1 小响应由 OpenResty 领先**：GET 1 KiB 的配对中位吞吐比为
   `lite/OpenResty = 0.825`，即 lite-nginx 低约 17.5%。
2. **HTTP/1.1 大响应和 echo 由 lite-nginx 明显领先**：64 KiB、1 MiB 和 POST echo
   1 MiB 的配对中位吞吐分别为 OpenResty 的 `1.713x`、`2.307x`、`1.699x`。
   去掉 CFS CPU quota 后重新测试，方向没有改变。
3. **HTTP/2 的吞吐中位数全部由 lite-nginx 领先**：1 KiB、64 KiB、1 MiB 和 POST
   echo 1 MiB 的配对中位比分别为 `1.140x`、`1.887x`、`1.579x`、`1.305x`。
   但 OpenResty 的 1 KiB 和 64 KiB 样本 CV 分别为 12.8% 和 13.7%，这两个精确倍数
   应视为本机趋势，不应外推为稳定生产差距。
4. **零丢包 HTTP/3 下两者总体接近**：单连接、单 stream 时，GET 1 KiB、64 KiB、
   1 MiB 的配对中位吞吐比分别为 `0.988x`、`1.024x`、`1.081x`。也就是 1 KiB
   基本持平，64 KiB 略高，1 MiB 约高 8.1%。
5. **HTTP/3 POST 1 MiB 不能下强结论**：追加 5 组零丢包样本后，lite-nginx 中位数
   为 71 RPS、OpenResty 为 74 RPS，配对中位比为 `0.918x`；但 lite-nginx CV
   仍为 14.8%，故只能说本批 OpenResty 中位数约高 4%～8%，结果不稳定。
6. **不能使用本机高并发 HTTP/3 大包结果**：`2 connections × 4 streams` 的
   64 KiB/1 MiB 样本出现 `UdpRcvbufErrors`。这些样本已判无效，最终 HTTP/3
   对比全部改用零丢包的 `c1 × m1` 批次。

## 2. 测试对象与工具

| 项目 | 实际版本/构建 |
|---|---|
| CPU | VMware 虚拟机内 4 vCPU，宿主型号显示为 13th Gen Intel Core i5-13500H |
| Kernel | Linux 6.17.0-40-generic x86_64 |
| lite-nginx | 当前工作树，Clang 20.1.8，Release `-O3 -DNDEBUG`，HTTP/3 开启，UDP GSO 关闭 |
| OpenResty | OpenResty 1.31.1.1 / nginx 1.31.1，GCC 15.2.0，`-O3 -DNDEBUG` |
| TLS | 两者均使用仓库 BoringSSL 和同一证书 |
| HTTP/1.1、HTTP/2 client | `/usr/bin/h2load`，nghttp2 1.64.0 |
| HTTP/3 client | 当前工作树构建的 `http3_benchmark_client` |
| backend | 当前工作树构建的 `http_benchmark_backend` |

本机 PATH 已有 `wrk` 和 `h2load`。HTTP/1.1/2 选择 h2load，是因为同一工具可固定
连接/stream 并输出逐请求延迟日志；HTTP/3 使用仓库客户端，因为它会逐请求校验状态码和
响应长度，并输出 QUIC/PTO/接收缓存诊断。

现有 OpenResty 二进制缺少 HTTP/2 模块，因此在 `temp/` 中重建了同时包含
`--with-http_v2_module` 和 `--with-http_v3_module` 的 OpenResty。仓库原 Release
依赖目录中的 BoringSSL 曾被 sanitizer 构建污染，本次另建
`build-benchmark-all/` 和 `temp/benchmark-all-deps/`；最终 lite-nginx 二进制没有
ASan/UBSan 未解析符号，也没有链接 sanitizer runtime。

关键二进制 SHA-256：

| 二进制 | SHA-256 |
|---|---|
| lite-nginx | `1dda62294840579a4a8e335850775e357c568a52bd89beff6350a71ad5db5e3b` |
| backend | `4e08fbf15e09c8f8008fdee9407ed34971b376abdff6c415598dc78278f6a071` |
| HTTP/3 client | `29a6f8645af95934a10c8f27d05f0f817f24c2755b3751c67fea8705c2175f9e` |
| OpenResty | `fb11e4de48f1de959d4b94d040f901bd6e939c07689195ea45b7793492bbc516` |
| h2load | `aeaef0d96c974290a472bdf2d3dc7c7f220ff30de4283163b51fc4a939d5d6be` |

## 3. 公平性设置

### 3.1 服务配置

- lite-nginx 和 OpenResty 都使用 2 个 worker。
- SUT 固定到 CPU 0、1；backend 固定到 CPU 2；client 固定到 CPU 3。
- backend 只接受 HTTP/1.1，两个 SUT 都复用到 backend 的 keepalive 连接。
- upstream pool 大小均为 256，access log、proxy buffering、request buffering 均关闭。
- lite-nginx `connection_pool steal off`，避免把 connection stealing 的独立行为混入对比。
- OpenResty HTTP/3 `quic_gso off`；lite-nginx 构建也关闭 UDP GSO。
- HTTP/3 client pacing 固定为 `on`，两种 SUT 使用同一个客户端设置。
- TLS 版本固定为 TLS 1.3，证书和 key 完全相同。

专用配置：

- `scripts/benchmark/all/configs/lite_nginx.conf`
- `scripts/benchmark/all/configs/openresty.conf`

### 3.2 端口与已有进程

原 benchmark 端口 18080 被用户已有的 `quantification` 进程占用。本测试没有停止或修改该
进程，而是改用：

- HTTP/1.1：`127.0.0.1:28080`
- HTTPS HTTP/2 和 QUIC HTTP/3：`127.0.0.1:28443`
- backend：`127.0.0.1:29001`

因此本报告属于“有正常后台进程的开发机本机对比”，不是裸机实验室基准。

### 3.3 负载与统计

每种协议均覆盖：

- GET 1 KiB；
- GET 64 KiB；
- GET 1 MiB；
- POST 1 MiB，并由 backend 原样 echo 1 MiB。

HTTP/1.1/2：

- 3 次交错重复，每次 2 秒 warmup + 8 秒 measurement；
- HTTP/1.1 为 128 clients；
- HTTP/2 为 4 connections × 16 streams；
- 每个样本重新启动 SUT；
- 报告 RPS、逐请求 p50/p99/p99.9、SUT CPU、内存、错误和退出状态；
- HTTP/1.1 大包使用无 CPU quota 复核批次作为最终数据；HTTP/1.1 1 KiB 和全部 HTTP/2
  样本的 throttled time 为 0。

HTTP/3：

- GET 为 3 次交错重复，POST 另取 5 次追加批次；
- 每次 2 秒 warmup + 8 秒 measurement；
- 最终为 1 connection × 1 stream；
- client 和 `nstat` 同时要求零失败、零接收缓存拒绝、零
  `UdpInErrors/UdpRcvbufErrors/UdpSndbufErrors`；
- 每个样本重新启动 SUT。

表中的 MiB/s 仅表示响应 payload。POST echo 同时还有相同大小的请求上传，因此线路总 payload
约为表值的两倍。

## 4. HTTP/1.1 结果

HTTP/1.1 使用明文监听，避免把 TLS 成本混入基础反向代理路径。

| 场景 | lite RPS | OpenResty RPS | 配对 lite/OR | lite payload MiB/s | OR payload MiB/s | lite p99 ms | OR p99 ms | lite/OR CPU 效率 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | 35,787 | 46,020 | 0.825x | 35.0 | 44.9 | 8.82 | 15.07 | 0.611x |
| GET 64 KiB | 16,611 | 9,607 | 1.713x | 1,038.2 | 600.4 | 16.14 | 23.86 | 1.989x |
| GET 1 MiB | 1,659 | 693 | 2.307x | 1,659.4 | 693.1 | 124.85 | 371.46 | 3.653x |
| POST echo 1 MiB | 736 | 429 | 1.699x | 736.3 | 429.1 | 234.77 | 567.17 | 3.152x |

解释：

- 1 KiB 是 OpenResty 的优势区间：RPS 和每 CPU 秒请求数都更高。
- 从 64 KiB 开始，lite-nginx 的 streaming proxy 路径在吞吐、p99 和 CPU 效率上同时领先。
- 无 quota 的 1 MiB GET 中 lite-nginx CV 为 12.9%，精确的 `2.307x` 有批次波动；但三组
  配对结果全部大于 `1.98x`，方向稳定。
- POST echo 的全双工 payload 约为 lite-nginx 1.44 GiB/s、OpenResty 0.84 GiB/s。

## 5. HTTP/2 结果

HTTP/2 使用 TLS 1.3 和 ALPN `h2`。功能门禁记录的实际 HTTP version 为 `2`。

| 场景 | lite RPS | OpenResty RPS | 配对 lite/OR | lite payload MiB/s | OR payload MiB/s | lite p99 ms | OR p99 ms | lite/OR CPU 效率 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | 34,488 | 30,947 | 1.140x | 33.7 | 30.2 | 4.49 | 4.42 | 0.912x |
| GET 64 KiB | 9,091 | 4,812 | 1.887x | 568.2 | 300.7 | 13.80 | 7.73 | 1.155x |
| GET 1 MiB | 817 | 520 | 1.579x | 816.9 | 520.1 | 376.58 | 376.98 | 1.557x |
| POST echo 1 MiB | 364 | 279 | 1.305x | 363.5 | 279.4 | 774.87 | 528.10 | 1.218x |

解释：

- lite-nginx 的吞吐中位数在四个场景均更高。
- GET 1 KiB 的 CPU 效率仍略低于 OpenResty；64 KiB 以上才同时取得 CPU 效率优势。
- 64 KiB 和 POST 1 MiB 的 lite p99 高于 OpenResty，说明这里的吞吐优势没有同步转化为
  更好的尾延迟。
- OpenResty GET 1 KiB、64 KiB 的 CV 为 12.8%、13.7%，1 MiB 为 6.3%；GET 64 KiB
  的 p99.9 也达到秒级。方向可用，但精确差距和极端尾延迟需要在独立机器上复测。

## 6. HTTP/3 结果

### 6.1 最终零丢包批次

| 场景 | lite RPS | OpenResty RPS | 配对 lite/OR | lite MiB/s | OR MiB/s | lite p99 ms | OR p99 ms | lite/OR CPU 效率 | 有效样本 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | 1,662 | 1,732 | 0.988x | 1.6 | 1.7 | 1.96 | 1.90 | 0.959x | 6/6 |
| GET 64 KiB | 1,000 | 955 | 1.024x | 62.5 | 59.7 | 3.05 | 3.04 | 1.034x | 6/6 |
| GET 1 MiB | 157 | 145 | 1.081x | 156.6 | 144.9 | 14.05 | 15.65 | 1.085x | 6/6 |
| POST echo 1 MiB | 71 | 74 | 0.918x | 71.1 | 74.2 | 27.78 | 27.39 | 0.947x | 10/10 |

GET 1 KiB、64 KiB、1 MiB 的 lite CV 分别为 5.4%、3.2%、3.2%，OpenResty 分别为
1.9%、3.8%、1.0%。GET 的结果足以支持“基本持平到 lite 小幅领先大响应”的本机结论。

POST 追加 5 组后，lite CV 仍有 14.8%，OpenResty 为 2.8%；配对比分布从 0.797x 到
1.114x。因此不应把 0.918x 当成稳定产品差距，只能记录本批中位数略偏 OpenResty。

所有最终 HTTP/3 样本：

- request failed = 0；
- phase error = 0；
- endpoint dropped datagrams = 0；
- receive storage rejected = 0；
- UDP kernel errors delta = 0；
- PTO count = 0。

### 6.2 被剔除的高并发批次

`2 connections × 4 streams` 下：

- GET 1 KiB 的 6 个样本有效；
- GET 64 KiB、GET 1 MiB、POST 1 MiB 共 18 个样本都出现正的
  `UdpInErrors/UdpRcvbufErrors`；
- 客户端虽完成全部请求且 application error 为 0，但内核已丢 UDP datagram，不能作为公平
  server capacity 结论。

原始无效数据仍保存在
`temp/all-benchmark-results/http3-allbench-final-20260729/`，用于说明为何降低并发，而没有
混入上表。

## 7. 有效性和限制

最终选用的 76 个正式样本（HTTP/1.1/2 共 48 个，HTTP/3 GET 18 个，HTTP/3 POST 10 个）：

- 0 request failure；
- 0 timeout；
- 0 SUT unexpected exit；
- HTTP/3 为 0 UDP error；
- 状态码和响应长度门禁通过。

仍有以下限制：

1. 只有 4 vCPU，client、proxy、backend 虽绑核但共享同一 VM、内存总线和 loopback。
2. 本机存在用户后台进程；结果适合当前开发机回归，不是跨机器容量上限。
3. HTTP/1.1 使用 128 clients，而 HTTP/2 使用 4×16 streams，HTTP/3 为了零丢包只能使用
   c1×m1；可以在同协议内比较实现，不能直接用不同协议的 RPS 排名协议本身。
4. HTTP/3 c1×m1 衡量低并发效率，不代表多核容量。要测容量，需要提高 UDP receive buffer
   或拆分 client/server 主机后重新寻找零丢包并发点。
5. 样本数为 3 或 5，报告使用配对中位数而非声称严格统计显著性；CV 超过 5% 的结论已明确
   降级。
6. OpenResty 和 lite-nginx 的 allocator、内部 buffering 与协议实现不同，这是产品整体对比，
   不是某个单函数 microbenchmark。

## 8. 复现

### 8.1 构建

```bash
cmake -S . -B build-benchmark-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_DEPS_DIR="$PWD/temp/benchmark-all-deps" \
  -DFETCHCONTENT_SOURCE_DIR_BORINGSSL="$PWD/temp/_deps/boringssl-src" \
  -DFETCHCONTENT_SOURCE_DIR_PROTOBUF="$PWD/temp/_deps/protobuf-src" \
  -DFETCHCONTENT_SOURCE_DIR_ZLIB="$PWD/temp/_deps/zlib-src" \
  -DFIBER_ENABLE_HTTP3=ON \
  -DFIBER_ENABLE_UDP_GSO=OFF

cmake --build build-benchmark-all \
  --target fiber_app_lite_nginx http_benchmark_backend http3_benchmark_client \
  -j4
```

OpenResty 的完整 configure 参数保存在每个结果目录的 `openresty-version.txt` 或
`nginx-version.txt` 中。测试二进制路径为：

```text
temp/openresty-all-benchmark/nginx/nginx/sbin/nginx
```

### 8.2 HTTP/1.1 和 HTTP/2

```bash
REPETITIONS=3 DURATION=8 WARMUP=2 COOLDOWN=1 \
IMPLEMENTATIONS="lite openresty" \
BACKEND_CPUS=2 SUT_CPUS=0,1 CLIENT_CPUS=3 LOAD_THREADS=1 \
BACKEND_QUOTA=none SUT_QUOTA=none \
H2_CLIENTS_OVERRIDE=4 H2_STREAMS_OVERRIDE=16 \
H2LOAD_BIN=/usr/bin/h2load \
LITE_NGINX_BIN="$PWD/build-benchmark-all/apps/lite_nginx" \
BACKEND_BIN="$PWD/build-benchmark-all/example/http_benchmark_backend" \
NGINX_BIN="$PWD/temp/openresty-all-benchmark/nginx/nginx/sbin/nginx" \
LITE_CONFIG="$PWD/scripts/benchmark/all/configs/lite_nginx.conf" \
NGINX_CONFIG="$PWD/scripts/benchmark/all/configs/openresty.conf" \
PLAIN_PORT=28080 TLS_PORT=28443 BACKEND_PORT=29001 \
scripts/benchmark/http/run_matrix.sh
```

### 8.3 HTTP/3

```bash
REPETITIONS=3 DURATION=8s WARMUP=2s COOLDOWN=1 \
CONNECTIONS=1 STREAMS=1 PACING=on \
BACKEND_CPUS=2 SUT_CPUS=0,1 CLIENT_CPUS=3 \
bash scripts/benchmark/all/run_http3_matrix.sh

scripts/benchmark/all/summarize_http3.py \
  temp/all-benchmark-results/http3-<run-id>
```

### 8.4 结果目录

最终数据：

- HTTP/1.1 1 KiB 和 HTTP/2：
  `temp/http-benchmark-results/allbench-final-h12-20260729/`
- HTTP/1.1 64 KiB 无 quota：
  `temp/http-benchmark-results/allbench-noquota-H1-P-64K-20260729/`
- HTTP/1.1 1 MiB 无 quota：
  `temp/http-benchmark-results/allbench-noquota-H1-P-1M-20260729/`
- HTTP/1.1 POST 1 MiB 无 quota：
  `temp/http-benchmark-results/allbench-noquota-H1-P-POST-1M-20260729/`
- HTTP/3 GET 零丢包：
  `temp/all-benchmark-results/http3-allbench-final-c1m1-20260729/`
- HTTP/3 POST 追加 5 组：
  `temp/all-benchmark-results/http3-allbench-final-post5-20260729/`

每个目录保留环境、配置、版本、二进制哈希、每次 client 输出、逐请求延迟、SUT journal、
cgroup CPU/内存和网络计数器快照。

## 9. 最终验证

- `./format_code.sh`：通过；
- benchmark shell 脚本 `bash -n`：通过；
- 3 个 Python 汇总脚本 `py_compile`：通过；
- Release 目标 `fiber_app_lite_nginx`、`http_benchmark_backend`、
  `http3_benchmark_client`：构建通过；
- OpenResty `nginx -t`：配置通过；
- `fiber_tests`：1387 项运行，1385 项通过，2 项因环境能力跳过；
- `lite_nginx_tests`：77/77 通过；
- `git diff --check`：通过。
