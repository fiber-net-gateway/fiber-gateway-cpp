# HTTP/3 benchmark client/server 压测问题记录

日期：2026-07-21  
关联设计：[HTTP/3 压测客户端设计](http3_benchmark_client.md)  
相关历史：[lite-nginx HTTP/3 压测问题根因与修复](lite_nginx_http3_benchmark_issue_root_cause_and_fixes.md)

## 1. 文档目的

本文记录使用 `example/http3_benchmark_client` 分别压测 OpenResty 和
`example/http3_benchmark_server` 时暴露的问题，供后续定位和修复使用。它保存的是可复现条件、
观测事实、当前代码路径、尚未确认的假设和验收条件，不把性能现象直接写成未经证实的协议根因。

最初记录本文时只修改了 benchmark client 的 pacing 默认值和命令行开关。H3BENCH-001 和
H3BENCH-002 均已于 2026-07-21 修复；H3BENCH-002 的 40k RPS 连续运行验收结果见 5.7。

## 2. 问题总览

| ID | 范围 | 状态 | 优先级 | 摘要 |
|---|---|---|---|---|
| H3BENCH-001 | benchmark client | 已修复 | P0 | 连接进入终止错误后，closed-loop lane 对同步 `Canceled` 无界重试，event loop 忙循环，duration/timeout/drain 失效 |
| H3BENCH-002 | benchmark server/QUIC ACK 路径 | 已修复 | P0 | 延迟 ACK 缺少到期唤醒，且与 ACK-eliciting 帧同包后静态 ACK 帧被错误保留在 sent queue，阻塞后续 ACK |
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

### 4.3 修复前代码机制

高置信机制位于修复前的 `BenchmarkWorker::run_lane()`：

1. closed-loop 每轮从 `EventLoop::current().now()` 读取时间；
2. `run_request()` 返回后只调用 `record_result()`；
3. 对 terminal `Canceled` 没有 break、共享 connection-failed 状态或 backoff；
4. 如果下一次 `send_request_header()`/`attach_local_stream()` 仍同步失败，coroutine 可以在同一
   event-loop turn 内再次进入循环。

`EventLoop::now()` 返回成员 `now_`。`now_` 只在 `EventLoop::run_once()` 的 poll/timer 边界刷新，
不是每次调用都读取 `steady_clock`。因此同步错误循环不返回下一次 `run_once()` 时，lane 看到的
时间保持不变，`now >= measurement_end_` 永远不能成立。stop monitor 本身依赖 `sleep(10ms)`，
同样无法获得 timer wakeup。

进一步排查确认还存在一个跨层状态缺口：`Http3Connection::accepting_requests()` 原本只检查 H3
`Running` 和 peer GOAWAY，没有检查底层 QUIC 是否仍允许创建新 stream。QUIC 已进入
`GracefulClosing`、`Closing`、`Draining` 或 `Closed` 时，`attach_local_stream()` 会同步返回
`Canceled`，但 lane 的 H3 前置检查仍可能通过，使上述循环稳定持续。

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

### 4.4 已实现修复

修复同时处理 terminal connection 和同步重试公平性，没有仅依赖错误枚举或计数器上限：

1. `Http3Connection::accepting_requests()` 同时要求 `quic_.accepting_new_streams()`，使公共 H3 状态契约
   与底层 transport 一致；所有 lane 直接读取权威状态，不维护可能失真的镜像标记。
2. `run_lane()` 在请求前和记录结果后检查 connection 状态。terminal connection 上每条 lane 最多
   记录当前在途请求的一次失败，然后停止创建请求。
3. connection 仍可用时，失败路径执行正值 `sleep(1ms)`，保证 timer、缓存时间和 stop monitor 能够
   前进；成功热路径不增加挂起。
4. 每条 lane 对连续 `StreamOrHeaderSend + NotSent` 使用 32 次上限，超过后停止 lane。
5. 文本和 JSON 汇总增加 `terminal_lane_stops` 与 `failure_guard_lane_stops`。
6. terminal connection 不自动重连，继续遵守第一版“不重放、不自动重试”的语义。

### 4.5 验收结果

- 新增单元测试覆盖“H3 仍为 `Running`、QUIC 已开始 shutdown”，确认不再接受新 request。
- 使用固定 Nginx 1.31.3 的默认每连接请求上限触发 GOAWAY，1、4、64 lane 分别报告
  `terminal_lane_stops=1/4/64`；64 lane 只记录 63 个 terminal request failure，没有无界增长。
- 构造持续同步 `StreamOrHeaderSend + NotSent`，单 lane 在总计 32 次失败后报告
  `failure_guard_lane_stops=1`，配置为 5 秒的运行约 0.3 秒自行退出。
- benchmark server 正常 128 B GET 验证 530/530 成功，两个 lane-stop 计数均为 0。
- focused HTTP/3 测试 35 项通过；设置 `FIBER_HTTP3_NGINX_PORT=9443` 后，固定 Nginx interop
  测试也通过。
- 原 OpenResty 64 KiB echo pacing-off 复现仍应在后续完整矩阵中重跑，确认触发连接异常时不会 hang。

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

### 5.4 根因

H3BENCH-002 由 QUIC ACK 发送路径中的两个问题叠加触发：

1. `handle_receive_result()` 识别出 application packet 可以延迟 ACK 后直接返回，没有为
   `max_ack_delay` 到期建立唤醒。连接在没有其他发送事件时不会重新进入 send pump。
2. ACK 与 STREAM/PING 等 ACK-eliciting 帧同包时，发送提交逻辑把静态 `ack_frame` 当作该包的
   丢包/拥塞记账载体放入 `sent_frames`。在对端确认或判丢之前，`ack_frame.queued` 一直为 true，
   后续 ACK 生成会提前返回；高请求率下 `send_ack_count` 持续增长而 send scheduler 最终报告无工作。

把 client 的本地 `max_ack_delay` 临时设为 0 后，同一 40k RPS 形状立即恢复到 199,996/200,000、
0 错误和 0 PTO。这一 A/B 结果与 stalled connection 上同时出现的 `send_ack=true`、静态 ACK 位于
`sent_frames`、ACK timer 未激活相互印证。

### 5.5 修复

- 为 application delayed ACK 增加独立 timer；接收路径按剩余 `max_ack_delay` 建立一次唤醒，ACK
  发送、连接关闭和连接销毁路径负责取消。
- ACK 帧在 UDP 发送成功后立即释放，不再进入 loss recovery 的 `sent_frames`。同包中第一个真正的
  ACK-eliciting 帧承担 packet length 和 congestion/loss accounting，静态 ACK 帧可供下一次 ACK 复用。
- PTO 仍使用独立 loss-detection timer。固定 Nginx 1.31.3 同样用 `qc->push` 处理 delayed ACK，
  用单独的 `qc->pto` 处理 PTO；两类 deadline 不共用一个 timer entry。
- PTO 的两个 PING probe 分别编码为两个 QUIC packet，避免在 packet builder 中合并为一个 probe。

### 5.6 验收标准

- 40k RPS、1 KiB、4 connections × 8 streams，连续 5 轮均完成至少 99.9% offered 请求，
  0 timeout/terminal error。
- 第二至第五轮吞吐不得低于首轮 95%，p99 不得出现数量级增长。
- 每轮结束后 active connection/stream、send ticket 和 frame pool 使用量回到基线。
- 在 1% 受控 UDP loss 下仍能持续推进，不出现所有 lane 同时等待到 request timeout。
- 64 KiB 和 1 MiB case 在 pacing on 下保持 0 错误；性能优化不得只修复 1 KiB。
- benchmark server 关闭和重启 5/5 成功，无残留 UDP listener、assert、hang 或无法 bind。

### 5.7 本次验收结果

在同一个全新 server 进程上按 5.2 的命令连续运行 5 轮：

| 轮次 | succeeded / offered | completion | throughput | request/IO error | PTO |
|---:|---:|---:|---:|---:|---:|
| 1 | 199,995 / 200,000 | 99.9975% | 39,999.0 req/s | 0 / 0 | 0 |
| 2 | 199,981 / 200,000 | 99.9905% | 39,996.2 req/s | 0 / 0 | 0 |
| 3 | 199,986 / 200,000 | 99.9930% | 39,997.2 req/s | 0 / 0 | 0 |
| 4 | 199,994 / 200,000 | 99.9970% | 39,998.8 req/s | 0 / 0 | 0 |
| 5 | 199,994 / 200,000 | 99.9970% | 39,998.8 req/s | 0 / 0 | 0 |

五轮均满足 40k RPS 主验收条件，且第二至第五轮吞吐均高于首轮的 99.99%。1% UDP loss、64 KiB/
1 MiB 扩展回归和 server 资源高水位回基线仍需单独验证，不包含在本次结果中。

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
1. H3BENCH-001 client terminal-error busy loop（已完成）
   ↓ 确保失败场景有界、统计可信
2. H3BENCH-002 server no-progress instrumentation
   ↓ 固定速率、单连接到多连接逐级复现
3. 定位 send scheduler / pacing / MAX_STREAMS / cleanup 中的实际停滞点
   ↓ 加入单元和集成回归
4. 重新执行 OpenResty 与 benchmark server 完整矩阵
   ↓
5. 再评估 pacing 参数和大响应性能优化
```

在第 2 步完成前，不应根据当前大响应差距直接重写拥塞控制、全局关闭 server pacing，或宣称
OpenResty 与本项目的最终性能比例。
