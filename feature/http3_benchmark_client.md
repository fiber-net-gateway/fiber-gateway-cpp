# HTTP/3 压测客户端设计

## 1. 目标与定位

在 `example/` 下增加 `http3_benchmark_client`，直接复用仓库现有的 `Http3Client`、
`Http3ClientConnection`、`ClientHttp3Exchange` 和 `QuicUdpEndpoint`，用于：

- HTTP/3 client/server 功能和互操作 smoke test；
- 多连接、多 request stream 下的吞吐、延迟和生命周期回归；
- 开发期 profiling，以及变更前后的同机趋势比较；
- GET、固定请求体 POST 和响应完整性检查。

它不是 lite-nginx 与其他实现之间的独立性能裁判。压测 lite-nginx 时，客户端和服务端可能共享
本仓库的 QUIC/HTTP/3 实现，正式横向比较仍使用 `scripts/benchmark/http3/` 下固定版本的外部
h2load。内部客户端的结果只能说明当前客户端和服务端组合在指定环境下的行为。

## 2. 第一版范围

第一版支持：

- `https://host[:port]/path` URL，QUIC v1、TLS 1.3 和 ALPN `h3`；
- 启动前解析一次目标地址，以及用于确定性测试的 `--connect-to IP:PORT`；
- GET 和 POST，POST body 在启动阶段从文件读取；
- 多 event-loop 线程、总连接数和每连接固定并发 lane；
- closed-loop 和固定 RPS 两种调度模式；
- 预热、测量、请求超时和独立 drain 时间；
- 期望状态码和可选响应长度校验；
- TTFB、完整响应延迟、固定速率调度延迟及 corrected latency；
- 文本摘要和可选 JSON 摘要；
- SIGINT/SIGTERM 后停止创建请求，并在有界时间内结束在途请求；
- 请求阶段、`IoErr`、HTTP 状态类别和 `Http3RequestOutcome` 分类。

第一版不实现：

- 0-RTT、自动重试、自动重连或请求重放；
- 动态 QPACK、server push、代理和 origin coalescing；
- 每请求日志和无界原始延迟样本；
- 当前 QUIC 公共 API 未提供的累计 loss/retransmission 统计；
- short-connection、Retry、session resumption 等握手专项模式。

不自动重试是刻意的约束。重试会改变请求量和延迟含义，`NotSent`、`Rejected`、
`PossiblyProcessed` 和 `Complete` 必须按原始结果分别统计。

## 3. 命令行

典型 closed-loop：

```bash
build/example/http3_benchmark_client \
  https://localhost:18443/bench/1k \
  --connect-to 127.0.0.1:18443 \
  --threads 4 \
  --connections 8 \
  --streams 64 \
  --mode closed \
  --warmup 10s \
  --duration 30s \
  --drain 10s \
  --timeout 5s \
  --expect-status 200 \
  --expect-bytes 1024 \
  --insecure
```

仓库 demo server 的临时证书仅用于本地测试，因此示例使用 `--insecure`。验证证书链时改用
`--ca-file FILE`，并确保目标证书在有效期内且包含匹配 URL host 的 SAN。

固定速率：

```bash
build/example/http3_benchmark_client \
  https://localhost:18443/bench/1k \
  --connect-to 127.0.0.1:18443 \
  --threads 4 --connections 8 --streams 64 \
  --mode rate --rps 20000 \
  --warmup 10s --duration 30s
```

POST：

```bash
build/example/http3_benchmark_client \
  https://localhost:18443/bench/echo \
  --connect-to 127.0.0.1:18443 \
  --method POST --body request_64k.bin \
  --expect-status 200 --expect-bytes 65536
```

`--connections` 是全局连接数，`--streams` 是每连接 lane 数。`--threads` 不能大于连接数。
为了让服务端 `SO_REUSEPORT` 的多个 shard 获得不同客户端四元组，客户端线程数应不小于希望
覆盖的服务端 shard 数。

## 4. 组件与所有权

```text
main
 ├─ BenchmarkOptions / Target
 ├─ RunCoordinator
 ├─ EventLoopGroup(N)
 └─ BenchmarkWorker × N
     ├─ QuicUdpEndpoint × 1
     ├─ Http3Client × 1
     ├─ Http3ClientConnection × 本 worker 连接数
     ├─ RequestLane × 连接数 × streams
     ├─ 共享只读 request body IoBuf
     └─ WorkerStats
```

每个 worker 归属一个 event loop，持有一个 client-only `QuicUdpEndpoint` 和一个
`Http3Client`。该 worker 的连接全部在同一 owner loop 创建和使用，避免跨线程访问 QUIC/H3
状态。连接按 round-robin 分配给 worker；同一 worker 的连接共享 UDP socket 和 TLS context。

`Http3Client` 和 endpoint 长于全部连接，连接长于其 lane 和 exchange。每个 request 使用独立
`BufPool`，pool 长于 exchange；放弃未完成请求时显式调用 `abort()`。

## 5. 启动与停止流程

1. main 解析参数、URL 和目标地址，读取请求体。
2. 启动 `EventLoopGroup`，向每个 owner loop 投递对应 worker。
3. worker 初始化 endpoint 和 client，并顺序建立其负责的连接。当前 `Http3Client::connect()`
   不作为同一 client 上的并发建连接口使用。
4. 所有 worker 完成连接阶段后，main 发布统一的 `warmup_start`；各 worker 使用相同的
   `measurement_start` 和 `measurement_end`。
5. worker 为每条连接启动固定数量的 lane，并等待全部 lane 结束。
6. 测量截止后不再发新请求；已开始请求最多运行到请求 deadline 和全局 drain deadline 的较早值。
   若 SIGINT/SIGTERM 提前终止测量，worker 在信号后的 drain deadline 主动关闭连接，唤醒仍阻塞在
   协议读写中的请求。
7. 采集 endpoint/connection 快照，向连接发送 graceful shutdown，等待 H3 reader/control task
   退出；graceful close 超过 drain 上限时统一切换为立即 H3 close，然后关闭 endpoint。
8. main 汇总 worker-local 结果，输出摘要并停止 event loops。

连接阶段只要有一个连接失败，整轮默认失败，避免把残缺并发的结果误当成目标配置。运行阶段
连接关闭后不自动重连，剩余请求按原始错误结束。

## 6. 请求 lane

### 6.1 Closed-loop

每条连接创建 `streams` 个长期 lane。一个 lane 同时只持有一个 exchange；请求完成后立即发下一
个请求。理论最大在途数为：

```text
connections × streams
```

如果 peer 广告的 QUIC 双向 stream credit 更小，`ClientHttp3Exchange` 内部的
`attach_local_stream()` 等待路径负责背压，等待时间包含在完整请求延迟中。

### 6.2 Fixed-rate

固定速率模式仍只使用这些 lane，不为每个 request `spawn` 新协程。所有 request ticket 映射到
绝对时间点：

```text
scheduled_at = warmup_start + global_ticket / rps
```

worker 使用交错的 global ticket，使多个线程不会在同一毫秒形成整齐突发。lane 空闲时领取下一个
ticket；如果已经晚于 `scheduled_at`，立即发送并记录 queue delay。如果测量结束时仍有未启动的
理论 ticket，它们计入 offered，但不计入 started。

固定速率同时报告：

- service latency：实际开始发送到响应 FIN；
- queue delay：计划时间到实际开始发送；
- corrected latency：计划时间到响应 FIN；
- offered、started、finished 和 completion ratio。

这样可以显式暴露客户端无法维持目标 RPS 的情况，避免只看实际发出请求的延迟。

## 7. 单请求流程

1. 创建 request-local `BufPool` 和 `ClientHttp3Exchange`。
2. 记录实际开始时间，发送 request HEADERS。
3. POST 使用预加载 `IoBuf` 的共享 slice 构造 `IoBufChain`，不按请求复制整个 body。
4. 跳过 informational HEADERS；收到 final HEADERS 时记录 TTFB 和状态码。
5. 循环读取 DATA 到 FIN，只累计长度，不保留响应 body。
6. 校验期望状态码和可选长度，记录 outcome 和延迟。
7. 任一步失败时记录当前 phase 和 `IoErr`，显式 abort exchange，不重试。

每次异步操作使用同一个绝对 request deadline 计算 remaining timeout，不能让 header、body 和每个
read chunk 各自重新获得完整超时时间。请求 deadline 还要受全局 drain deadline 限制。

## 8. 热路径与内存

- request lane 数量固定，运行期不创建无界 coroutine；
- request body 每 worker 只分配和复制一次，request 通过引用计数 slice 共享存储；
- `IoBufChain` 使用 owner loop 的 `IoBufNodePool`；
- worker 只写自己的计数器和 histogram，运行期没有跨 worker 统计锁；
- latency 使用固定容量、约 1/256 相对精度的对数 histogram，不保存全部样本；
- worker 热路径不打印、不写文件，main 在所有 worker 结束后统一输出；
- CLI、DNS、文件读取和 JSON 输出是冷路径，可以使用 `std::string`、`std::vector` 和流式 I/O。

当前 client 每个 exchange 会创建 `ClientHttp3Request`，响应 header 也需要 request-local
`BufPool`。第一版测量的是这套真实公共 API 的完整成本，不为压测器增加隐藏的专用快路径。执行
正式数据前必须确认客户端 CPU、内存和 UDP drop 尚有余量。

## 9. 统计

文本和 JSON 摘要至少包含：

- 目标 URL、解析地址、线程、连接、streams、模式和时间参数；
- offered、started、finished、succeeded、failed 和 completion ratio；
- status 1xx/2xx/3xx/4xx/5xx/other；
- response bytes、requests/s 和 MiB/s；
- 配置 duration 和实际 measurement elapsed；信号提前终止时，offered 和吞吐按实际测量区间计算；
- TTFB、total、queue delay、corrected latency 的 p50/p90/p99/p99.9/max；
- connect、stream/header send、body send、header read、body read、status 和 length 校验错误；
- `IoErr` 和 `Http3RequestOutcome` 分布；
- endpoint dropped datagrams、retained receive storage high-water/rejection；
- 连接 active path 收发字节、RTT、cwnd、in-flight、PTO 和 key generation 快照。

延迟 histogram 只记录协议完成并通过状态码/长度校验的正式测量请求。错误请求仍计入 started、
finished、错误分类和 outcome，但不污染成功请求的延迟分位数。

## 10. 退出状态

- `0`：运行完成且没有 setup/request/validation 错误；
- `1`：参数、解析、初始化、建连或其他 setup 失败；
- `2`：运行完成，但预热或正式测量存在请求/校验失败；
- `130`：收到 SIGINT/SIGTERM；仍应先输出已完成的摘要。

## 11. 验证计划

1. 构建 `http3_benchmark_client` 和 `http3_benchmark_server`。
2. 对 benchmark server 验证 1 KiB GET、64 KiB GET 和 64 KiB POST echo。
3. 覆盖 `c1×m1`、`c1×m64`、`c8×m64` closed-loop。
4. 固定 RPS 下核对 offered、started、finished 和调度延迟。
5. 验证证书失败、404、响应长度不匹配和请求超时。
6. 在测量中发送 SIGINT，确认停止发新请求、有界 drain 和摘要输出。
7. 对仓库固定 Nginx 执行 HTTP/3 smoke test。
8. 使用外部 h2load 交叉检查趋势，并确认内部客户端不是 CPU、内存或 UDP 路径瓶颈。
