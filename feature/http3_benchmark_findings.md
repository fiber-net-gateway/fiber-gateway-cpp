# HTTP/3 benchmark client/server 压测问题记录

日期：2026-07-21  
关联设计：[HTTP/3 压测客户端设计](http3_benchmark_client.md)  
相关历史：[lite-nginx HTTP/3 压测问题根因与修复](lite_nginx_http3_benchmark_issue_root_cause_and_fixes.md)

## 1. 文档目的

本文记录使用 `example/http3_benchmark_client` 分别压测 OpenResty 和
`example/http3_benchmark_server` 时暴露的问题，供后续定位和修复使用。它保存的是可复现条件、
观测事实、当前代码路径、尚未确认的假设和验收条件，不把性能现象直接写成未经证实的协议根因。

本轮只修改了 benchmark client 的 pacing 默认值和命令行开关，没有修复本文列出的两个 P0 问题。
在它们修复前，长时间 closed-loop 错误场景和 benchmark server 的峰值结果不能作为正式性能结论。

## 2. 问题总览

| ID | 范围 | 状态 | 优先级 | 摘要 |
|---|---|---|---|---|
| H3BENCH-001 | benchmark client | 已确认，机制高置信 | P0 | 连接进入终止错误后，closed-loop lane 对同步 `Canceled` 无界重试，event loop 忙循环，duration/timeout/drain 失效 |
| H3BENCH-002 | benchmark server/QUIC 发送路径 | 现象已确认，根因未确认 | P0 | 服务端经历持续请求或固定速率负载后停止推进响应和新 request stream，吞吐骤降并出现超时 |
| H3BENCH-003 | pacing/跨实现互操作 | 行为已确认 | P1 | client pacing off 可提高 loopback GET 峰值，但突发 POST 或双方 pacing off 可使连接失去进展 |
| H3BENCH-NOTE-001 | OpenResty 测试配置 | 已解释，不是项目缺陷 | 说明 | OpenResty/Nginx 默认每连接 1000 请求，未提高限制时预热会主动耗尽连接 |

这里的 P0 表示“会破坏压测工具可信度，必须先修复”，不表示已经确认生产服务存在安全漏洞。

## 3. 测试环境与证据

### 3.1 环境

```text
OS: Linux 6.17.0-40-generic
CPU: 4 vCPU, Intel Core i5-13500H, VMware
Memory: 3.3 GiB
Build: CMAKE_BUILD_TYPE=Release, clang 20.1.8
Network: 127.0.0.1 loopback
Client: build-release/example/http3_benchmark_client
Server: build-release/example/http3_benchmark_server
OpenResty: 1.31.1.1, nginx core 1.31.1, BoringSSL
OpenResty source archive SHA-256: 65b78baadd3f0984055de89bf13f4a1932e5bfe9c31932037a134ea2b1a0ce42
Pinned nginx reference: 1.31.3, scripts/build_nginx.sh
```

client 默认 pacing off；为了使结果可复现，本文命令仍显式写出 `--pacing off` 或
`--pacing on`。benchmark server 除专门的 off 实验外均使用 pacing on。

### 3.2 原始结果

原始 JSON 位于 `temp/http3-benchmark/`。该目录被 Git 忽略，可能被清理，因此关键数值已经复制到
本文；后续排查应使用本文命令生成新的独立 `RUN_ID` 结果，不应假设旧 JSON 永久存在。

主要证据文件：

```text
openresty-functional-echo64k.json  client 同步错误忙循环
fiber-server-off-1m-c2s2.json     双方 pacing off 后的同步错误忙循环
fiber-fresh-1k-c4s8.json          benchmark server 首轮正常高吞吐
fiber-after-1mstreams-128b.json   同一 server 后续负载退化
fiber-after-grace-128b.json       等待 32 秒后的部分恢复
fiber-rate-40k.json               独立 40k RPS 停止推进
openresty-rate-40k.json           相同 client 的固定速率正常对照
openresty-rate-80k.json           rate 模式的可控饱和对照
```

## 4. H3BENCH-001：client 终止错误后的同步忙循环

### 4.1 现象

当连接在 closed-loop 请求期间失去进展并使后续请求同步返回 `IoErr::Canceled` 时，lane 不会停止，
也没有退避或保证返回下一轮 event loop。结果是：

- 单个或多个 worker 长时间占满一个 CPU；
- `--duration`、`--timeout` 和 `--drain` 已经过期，进程仍不退出；
- 统计中的 `started`/`failed` 以每秒数千万次增长，但这些不是实际发到网络的请求；
- 同一 event loop 上的 stop monitor 和 request timer 得不到执行机会；
- SIGINT/SIGTERM 的原子标记仍能在下一次 lane 外层判断中生效，实测 SIGINT 后约 1 秒退出。

OpenResty 64 KiB echo 的一轮结果为：

```text
warmup_started=1,123,646,741
warmup_errors=1,123,646,118
warmup stream_or_header_send=1,123,646,110
warmup canceled=1,123,646,118
measurement started=0
```

benchmark server pacing off、1 MiB GET 的另一轮结果为：

```text
warmup_started=364,489,658
warmup_errors=364,489,467
measurement started=359,510,057
measurement succeeded=19
measurement failed=359,510,038
measurement canceled=359,510,038
```

这些计数代表本地同步失败循环，不代表服务端处理了对应数量的 HTTP 请求。

### 4.2 最小复现

OpenResty 使用 64 KiB 内存 request body echo，配置至少包括：

```nginx
worker_processes 4;

events {
    worker_connections 65535;
}

http {
    access_log off;

    server {
        listen 127.0.0.1:19443 quic reuseport;
        http3 on;
        keepalive_requests 100000000;
        keepalive_time 1h;

        ssl_certificate /path/to/build/http3-demo/cert.pem;
        ssl_certificate_key /path/to/build/http3-demo/key.pem;
        ssl_protocols TLSv1.3;

        client_max_body_size 2m;
        client_body_buffer_size 2m;

        location = /bench/echo {
            default_type application/octet-stream;
            content_by_lua_block {
                ngx.req.read_body()
                local body = ngx.req.get_body_data() or ""
                ngx.header.content_length = #body
                ngx.print(body)
            }
        }
    }
}
```

请求体和复现命令：

```bash
mkdir -p temp/http3-benchmark
truncate -s 65536 temp/http3-benchmark/body-64k.bin

timeout --signal=INT --kill-after=2s 8s \
  build-release/example/http3_benchmark_client \
  https://localhost:19443/bench/echo \
  --connect-to 127.0.0.1:19443 \
  --threads 1 --connections 1 --streams 4 \
  --mode closed \
  --warmup 100ms --duration 1s --drain 1s --timeout 2s \
  --method POST --body temp/http3-benchmark/body-64k.bin \
  --expect-status 200 --expect-bytes 65536 \
  --insecure --pacing off
```

该最小形状的一轮 off 结果在 watchdog 触发前已累计约 5220 万次 warmup 尝试；把同一命令改为
`--pacing on` 后稳定完成 439 req/s，0 错误。pacing 只负责触发连接异常的概率和时序，连接异常后
无界同步重试属于独立的 client 缺陷。

### 4.3 当前代码机制

高置信机制位于 `BenchmarkWorker::run_lane()`：

1. closed-loop 每轮从 `EventLoop::current().now()` 读取时间；
2. `run_request()` 返回后只调用 `record_result()`；
3. 对 terminal `Canceled` 没有 break、共享 connection-failed 状态或 backoff；
4. 如果下一次 `send_request_header()`/`attach_local_stream()` 仍同步失败，coroutine 可以在同一
   event-loop turn 内再次进入循环。

`EventLoop::now()` 返回成员 `now_`。`now_` 只在 `EventLoop::run_once()` 的 poll/timer 边界刷新，
不是每次调用都读取 `steady_clock`。因此同步错误循环不返回下一次 `run_once()` 时，lane 看到的
时间保持不变，`now >= measurement_end_` 永远不能成立。stop monitor 本身依赖 `sleep(10ms)`，
同样无法获得 timer wakeup。

相关位置（以后续代码中的函数名为准，行号可能漂移）：

```text
example/http3_benchmark_client.cpp
  BenchmarkWorker::run_lane
  BenchmarkWorker::monitor_stop
  BenchmarkWorker::record_result

src/event/EventLoop.h
  EventLoop::now

src/event/EventLoop.cpp
  EventLoop::run_once
```

### 4.4 修复方向

建议同时处理 terminal connection 和同步重试公平性，不能只在计数器上加上限：

1. 每条 connection 增加 worker-local 终止标记；任一 lane 观察到 transport/H3 已终止后发布，
   该连接上的全部 lane 停止创建请求。
2. 区分 request-local error 和 terminal connection error。`Canceled`、`ConnReset`、
   `NotConnected`、`BrokenPipe` 不能仅凭枚举值一刀切，应结合 H3/QUIC connection state 判断。
3. 非 terminal 的同步失败若允许继续采样，必须保证控制权回到下一次 `run_once()`，使缓存时间、
   timer 和 stop monitor 前进；仅做同一 turn 的 repost 不够。
4. 给每 lane/connection 增加有界的连续同步失败计数，超过阈值后停止该 lane，并在汇总中单独输出
   `terminal_lane_stops`，防止错误数掩盖真实 offered load。
5. terminal connection 不自动重连，继续遵守第一版“不重放、不自动重试”的语义。

### 4.5 验收标准

- 构造已终止连接，使 1、4、64 个 lane 的第一个请求同步返回 `Canceled`；每个 lane 最多记录一个
  terminal 失败，然后退出。
- `duration=1s, drain=1s` 的失败场景应在 3 秒内退出，不依赖外部 signal。
- 失败计数与 lane 数同阶，不能随 CPU 速度增长到百万级。
- stop monitor 必须获得运行机会并能在 drain deadline 强制关闭阻塞请求。
- closed-loop、rate 模式均覆盖；正常成功路径的 RPS 和分配数量不能因修复显著回退。
- OpenResty echo pacing off 的复现可以报告正常请求错误或连接终止，但不能 hang 或忙循环。

## 5. H3BENCH-002：benchmark server 持续负载后停止推进

### 5.1 现象 A：同一 server 的连续运行退化

全新 benchmark server、server pacing on，第一轮 1 KiB closed-loop：

```text
workers=4
client threads=4, connections=4, streams=8, pacing=off
warmup=2s, duration=5s
warmup_started=338,837, warmup_errors=0
measurement succeeded=740,275, failed=0
throughput=148,055 req/s
```

同一 server 进程紧接着用相同并发形状跑 128 B：

```text
warmup_started=125,370, warmup_errors=24
measurement succeeded=13,662, failed=32
throughput=2,732.4 req/s
errors: stream_or_header_send=16, header_read=16, timed_out=32
```

空闲 32 秒后再次运行只能部分恢复：

```text
throughput=52,700 req/s
warmup_errors=16
measurement failed=8, all header_read timed_out
```

重启 server 后曾重新恢复到 148,055 req/s，说明退化不是客户端二进制永久状态；但后续独立运行
仍能再次触发停止推进，根因尚不能简单归结为旧连接 drain。

### 5.2 现象 B：独立固定速率运行停止推进

启动全新 server：

```bash
build-release/example/http3_benchmark_server \
  18443 4 build/http3-demo/cert.pem build/http3-demo/key.pem on
```

以低于已观察峰值的 40k RPS 运行：

```bash
build-release/example/http3_benchmark_client \
  https://localhost:18443/bench/1k \
  --connect-to 127.0.0.1:18443 \
  --threads 4 --connections 4 --streams 8 \
  --mode rate --rps 40000 \
  --warmup 1s --duration 5s --drain 2s --timeout 5s \
  --expect-status 200 --expect-bytes 1024 \
  --insecure --pacing off \
  --json temp/http3-benchmark/fiber-rate-40k.json
```

观测结果：

```text
offered=200,000
started=336
succeeded=304
failed=32
completion=0.00168
warmup_started=21,349
warmup_errors=24
measurement timed_out=32
```

相同 client、相同 `threads=4/connections=4/streams=8` 对 OpenResty 运行 40k RPS 时完成
199,997/200,000，0 错误；80k 目标仍完成 384,246 个请求且无请求错误，只表现为 queue delay
和完成率下降。因此 client 的 rate ticket 生成和统计路径可以正常工作，问题与 benchmark server
组合相关，但这还不能区分 server 本身、双方 QUIC 交互和本机 UDP 丢包的具体责任。

### 5.3 与大响应性能的关系

低并发、无错误的 server pacing on 基线为：

| Case | 并发形状 | throughput | total p99 | 错误 |
|---|---|---:|---:|---:|
| GET 64 KiB | 2 threads / 2 connections / 4 streams | 1,745.33 req/s，109.08 MiB/s | 10.528 ms | 0 |
| GET 1 MiB | 2 threads / 2 connections / 2 streams | 100 req/s，100 MiB/s | 104.448 ms | 0 |

同形状 OpenResty 为 841.31 MiB/s 和 901.5 MiB/s。这个差距说明 server 发送路径值得 profiling，
但不能仅凭吞吐差距确定 H3BENCH-002 的根因。关闭 server pacing 的 1 MiB 实验很快进入
H3BENCH-001 的同步 `Canceled` 忙循环，只成功 19 个请求，因此 server pacing off 不是可用修复。

### 5.4 当前尚未确认的根因

现有 client 摘要只能看到连接末尾快照，没有 server 侧连接状态和最后一次发送进展。以下是假设，
按建议排查优先级排列，不是结论：

1. **send scheduler/pacing timer 丢失唤醒**：连接仍非 terminal，但 pending response、
   `MAX_STREAMS` 或 ACK 没有再次进入 send pump。
2. **peer stream retirement/MAX_STREAMS 回补停滞**：短请求大量退休后，server 的
   `PeerStreamLimitWindow` 中 `opened_count`、`retired_count`、`advertised_limit` 不再一致，
   client 一部分 lane 阻塞在新 stream，另一部分等待已有 stream response。
3. **output frame/send ticket 泄漏**：大量短 stream 后 frame pool 或 stream send ticket 没有释放，
   新控制帧和响应帧无法排队。
4. **连接 close/drain 残留状态**：旧连接仍留在 endpoint routing/send queue 中，影响新连接公平性；
   32 秒后的部分恢复与此方向相关，但不能单独证明。
5. **worker/reuseport 分布或 endpoint 调度不公平**：少数 worker 失去发送进展后拖住其连接。
6. **主机 UDP receive buffer 丢包放大**：本机 `/proc/net/snmp` 曾观察到累计 `RcvbufErrors`，
   但现有 JSON 没有 server 每轮前后增量。QUIC 正常应能从少量丢包恢复，仍需受控排除该环境变量。

当前可以排除或暂不支持的解释：

- 不是 OpenResty 的 `keepalive_requests=1000`，问题目标是仓库 benchmark server；
- 不是单纯“40k 已超过 server 容量”，因为同一 build 曾在相近形状完成 148k req/s；
- client 侧 endpoint `recv_storage_rejected` 在上述关键结果中为 0，没有接收存储预算耗尽证据；
- stall 前的 HTTP status 和 response length 校验通过，不是固定响应内容错误。

### 5.5 下一轮需要增加的观测

在继续调参或改算法前，应让 benchmark server 在每连接无进展阈值触发时输出一次固定大小快照：

```text
connection state / close source / H3 state
active request stream count
peer bidi opened_count / retired_count / advertised_limit / concurrent_limit
local bidi next stream id / peer MAX_STREAMS limit
pending/inflight/acked/lost packet and frame counts
send scheduler queued flag / stream send ticket count
pacing enabled / pacing timer armed / deadline / budget / capacity / rate
cwnd / bytes_in_flight / smoothed RTT / PTO count
output frame pool allocated/free/high-water
endpoint send queue length / dropped datagrams / UDP socket errors
last receive time / last successful packet build / last successful send time
```

建议排查步骤：

1. 先修复 H3BENCH-001，避免 client 错误循环污染 server 现象。
2. 使用 rate 模式从 5k、10k、20k、40k 逐级增加，分别跑 1 connection/1 stream、
   1 connection/8 streams、4 connections/8 streams。
3. 每轮前后记录 `/proc/net/snmp` 的 `Udp: InErrors/RcvbufErrors/SndbufErrors` 增量；有增量的点不用于
   协议根因判断。
4. 在 stall 时抓 server 快照和 packet trace，判断最后一个可见事件是 `MAX_STREAMS`、response
   STREAM、ACK、pacing deadline 还是 socket write block。
5. 固定 client pacing on 再复现一次，以受控速率排除 off burst；只有两种 client pacing 都停滞时，
   才把重点收敛到 server 的稳态发送/stream retirement。
6. 分别测试 server pacing on/off，但 off 测试必须用较低固定 RPS，不能使用无界 closed-loop 直接
   冲击大响应。
7. 同一 server 连续运行至少 5 轮，并与“每轮重启 server”对照，区分连接内、endpoint 级和进程级
   残留状态。

### 5.6 验收标准

- 40k RPS、1 KiB、4 connections × 8 streams，连续 5 轮均完成至少 99.9% offered 请求，
  0 timeout/terminal error。
- 第二至第五轮吞吐不得低于首轮 95%，p99 不得出现数量级增长。
- 每轮结束后 active connection/stream、send ticket 和 frame pool 使用量回到基线。
- 在 1% 受控 UDP loss 下仍能持续推进，不出现所有 lane 同时等待到 request timeout。
- 64 KiB 和 1 MiB case 在 pacing on 下保持 0 错误；性能优化不得只修复 1 KiB。
- benchmark server 关闭和重启 5/5 成功，无残留 UDP listener、assert、hang 或无法 bind。

## 6. H3BENCH-003：pacing off 的收益与跨实现风险

### 6.1 已确认收益

OpenResty 1 KiB、2 connections × 8 streams、2 秒测量：

| client pacing | throughput | total p99 | 错误 |
|---|---:|---:|---:|
| off | 48,960 req/s | 1.040 ms | 0 |
| on | 40,852 req/s | 1.140 ms | 0 |

loopback 上 off 吞吐提高约 19.8%，支持 benchmark client 以 off 作为峰值测试默认值。

### 6.2 已确认风险

- OpenResty 64 KiB POST echo、1 connection × 4 streams：client pacing on 稳定 439 req/s、
  0 错误；off 使连接失去进展并触发 H3BENCH-001。
- benchmark server 64 KiB POST echo、1 connection × 4 streams：client off 曾稳定完成
  1,036 req/s；on 只完成 180 req/s 且出现 4 个 body-send timeout。说明 pacing 与当前仓库发送调度
  的组合也存在明显性能差异，不能用单个 case 推导全局默认。
- benchmark server pacing off、client pacing off 的 1 MiB GET 不能稳定完成，因此生产 server 的
  pacing 默认值不能跟随 benchmark client 一起关闭。

### 6.3 使用规则

1. 每份报告必须写出 client pacing 和 server pacing，不能依赖隐含默认值。
2. 峰值测试可以 client pacing off，但在正式计时前必须通过 response status/length/hash gate。
3. 只要出现 peer close、timeout、PTO 或 UDP drop，就必须以 client pacing on 复测。
4. 跨实现排名至少同时报告一个可比的 pacing on 点，避免把目标端 burst tolerance 当作业务吞吐。
5. benchmark client 默认 off 只是工具定位，不是生产 QUIC 发送策略建议。

## 7. H3BENCH-NOTE-001：OpenResty 请求上限测试陷阱

初始 OpenResty smoke test 使用 2 条连接，在约 2014 次 warmup 请求后连接关闭，表现为
`ConnReset/Canceled`。pacing on/off 都在相同计数附近发生，原因不是 pacing。

OpenResty 1.31.1.1 的 nginx core 默认：

```text
temp/openresty-1.31.1.1/build/nginx-1.31.1/
  src/http/ngx_http_core_module.c:3945-3946
  keepalive_requests default = 1000

temp/openresty-1.31.1.1/build/nginx-1.31.1/
  src/http/v3/ngx_http_v3_request.c:200-223
  request sequence 达到 keepalive_requests 时发送 GOAWAY；达到两倍时按 excessive load 关闭
```

仓库固定 Nginx 1.31.3 在
`temp/nginx-1.31.3/src/http/ngx_http_core_module.c:3940-3941` 使用相同默认值。

基准测试配置必须显式设置：

```nginx
keepalive_requests 100000000;
keepalive_time 1h;
```

设置后 OpenResty 的 GET、large response 和 rate case 均可持续运行。这个配置只用于长连接压测，
不能不经评估直接复制到生产配置。

## 8. 后续处理顺序

```text
1. H3BENCH-001 client terminal-error busy loop
   ↓ 确保失败场景有界、统计可信
2. H3BENCH-002 server no-progress instrumentation
   ↓ 固定速率、单连接到多连接逐级复现
3. 定位 send scheduler / pacing / MAX_STREAMS / cleanup 中的实际停滞点
   ↓ 加入单元和集成回归
4. 重新执行 OpenResty 与 benchmark server 完整矩阵
   ↓
5. 再评估 pacing 参数和大响应性能优化
```

在第 1、2 步完成前，不应根据当前大响应差距直接重写拥塞控制、全局关闭 server pacing，或宣称
OpenResty 与本项目的最终性能比例。
