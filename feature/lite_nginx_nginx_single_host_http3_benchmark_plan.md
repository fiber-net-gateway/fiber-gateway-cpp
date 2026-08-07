# lite-nginx 与 Nginx 单机 HTTP/3 性能及健壮性压测计划

状态：已执行（2026-07-19）；POST 正确性和连续 key update 前置门槛未通过，后续正式阶段按停止规则终止

制定日期：2026-07-19

执行结果：[HTTP/3 压测报告](lite_nginx_nginx_single_host_http3_benchmark_report.md)。下文复选项保留为
原始验收清单；实际完成项、无效轮次和未执行项以报告为准。

执行基线补充：计划制定后合入的 `13b8dfa` 已为 lite-nginx 增加 Linux UDP GSO、
`sendmmsg` 批发送及运行时回退。因此正式等价主表使用
`FIBER_ENABLE_UDP_GSO=OFF` / `quic_gso off` / 客户端 `--no-udp-gso`，另以双方均开启
GSO 的结果作为能力诊断。独立性能客户端已固定为 nghttp2 h2load 1.69.0（ngtcp2
1.24.0 + nghttp3 1.17.0），协议校验和主动 key update 使用同版本 ngtcp2 的
`bsslclient`。

前序基线：[HTTP/1、HTTP/2 单机压测计划](lite_nginx_nginx_single_host_benchmark_plan.md) / [执行报告](lite_nginx_nginx_single_host_benchmark_report.md)

## 1. 目标与结论边界

本计划在当前唯一可用的 WSL2 开发机上完成三类验证：

1. 在相同 CPU 预算、相同 TLS、相同 HTTP/1.1 上游下，对比 lite-nginx 与仓库固定版
   Nginx 的 HTTP/3 吞吐、尾延迟、CPU 效率和内存占用；
2. 单独测量 QUIC 完整握手、连接数和并发 stream 分布对结果的影响，避免把
   `SO_REUSEPORT` 分片偶然性解释成协议处理能力；
3. 检查 lite-nginx 在连接/stream churn、集中断连、过载、丢包/乱序、协议边界输入、
   上游异常和长时间运行下的健壮性，并以 Nginx 的观察结果作为互操作参考。

这里的“健壮性”是协议实现和服务生命周期验证，不是安全测试。所有流量只走本机
loopback；异常输入有显式速率、并发、内存和总时限，不进行外网扫描或无界资源消耗。

单机结果只允许用于：

- 同机、同客户端、同配置、同 CPU 预算下的 lite-nginx/Nginx 配对比较；
- 定位 QUIC/HTTP/3 热点、连接分片、内存增长和恢复能力；
- 建立可重复的开发基线和后续优化回归门槛。

不得据此推导生产公网容量、真实 NIC 吞吐、跨地域丢包表现或多机扩展能力。WSL2 的
loopback、内核 UDP 路径和宿主机调度都会影响绝对值，主要结论应使用配对比值和趋势，
而不是单次绝对 RPS。

## 2. 已确认的实现与环境事实

### 2.1 固定 Nginx 基线

- `scripts/build_nginx.sh:15` 固定 Nginx `1.31.3`；源码和二进制分别为
  `temp/nginx-1.31.3/` 与 `temp/nginx-install/sbin/nginx`；
- 当前源码和二进制均已存在，`nginx -V` 确认启用了 `--with-http_v3_module`，并使用
  项目构建的 BoringSSL；
- Nginx HTTP/3 listener 使用 `listen <port> quic reuseport`，见
  `scripts/nginx.conf:60-65`；
- `http3_max_concurrent_streams` 默认 128，`http3_stream_buffer_size` 默认 65536，
  `quic_retry` 和 `quic_gso` 默认关闭，见
  `temp/nginx-1.31.3/src/http/v3/ngx_http_v3_module.c:229-251`；
- Nginx 默认广告 4096 字节 QPACK 动态表和最多 128 个 blocked streams，见
  `temp/nginx-1.31.3/src/http/v3/ngx_http_v3_module.c:196-201,233-240`。

版本、源码和运行二进制继续以 `scripts/build_nginx.sh` 为唯一来源，不换用系统 Nginx。

### 2.2 lite-nginx HTTP/3 基线

- `listen <port> ssl http3` 同时建立 TCP TLS listener 和 UDP QUIC listener；
- `worker_processes=N` 时，HTTP/3 创建 N 个 UDP endpoint，每个 endpoint 归属一个
  worker event loop，并自动启用 `SO_REUSEPORT`，见
  `src/http/Http3Server.cpp:130-171,245-249`；
- 默认每 shard 最多 1024 个连接，默认双向/单向 stream 上限均为 128，stream 接收窗口
  为 65536 字节，见 `include/fiber/http/HttpExchange.h:31-46` 和
  `include/fiber/quic/QuicConnection.h:47-53,121-140`；
- 默认 HTTP/3 SETTINGS 将 QPACK 动态表容量和 blocked streams 都设为 0，见
  `include/fiber/http/Http3Protocol.h:31-36`；
- Retry、NEW_TOKEN 和 0-RTT 默认关闭；QUIC v1、peer 发起的 key update、旧密钥
  `3×PTO` 宽限、pacing、ECN、Retry/token、CID/path 和流控代码均已存在；
- lite-nginx 配置目前只暴露 `http3` listener 开关，没有暴露 QUIC flow-control、Retry、
  0-RTT、连接数和 QPACK 参数。正式执行若需要改变这些参数，应增加明确的配置项或
  benchmark 专用启动入口，禁止只在源码中暗改默认值。

### 2.3 当前已知限制

1. 本机当前没有 `h3load`、`ngtcp2-client` 或 `nghttp3-client`，系统 curl 8.5.0 也没有
   HTTP/3；不能用 HTTP/2 的 h2load 冒充 HTTP/3 压测器。
2. `feature/quic_audit.md` 记录 lite-nginx 尚无服务端主动 key update 和每代 AEAD 包计数；
   固定版 Nginx 1.31.3 也只响应 peer 发起的 key update。持续使用同一代 AES-GCM 密钥
   不能越过 RFC 9001 的保密性包数上限，因此压测客户端必须能主动轮换密钥，或在保守
   包数前重建连接。
3. lite-nginx 已支持 Linux UDP GSO 和 `sendmmsg` 批发送，但没有 `recvmmsg` 接收批处理；
   为了建立可与实现变更前后衔接的等价基线，双方正式主结果都关闭 GSO。另跑双方均开启
   GSO 的能力诊断，不混入主排名。
4. 前序 HTTP/2 压测在默认上游池 `steal auto` 下复现过集中断连崩溃。HTTP/3 会复用
   同一代理和连接池路径，执行本计划时必须先重跑 churn 门槛；未通过时默认配置的
   健壮性结论直接为失败，`steal off` 性能只能标为稳定回退诊断结果。
5. 当前 WSL2 UDP socket 上限为 `net.core.rmem_max=212992`、
   `net.core.wmem_max=212992`。不在计划中静默修改系统参数；每轮采集 UDP drop/error，
   一旦发生 buffer drop，该轮无效并降低负载或在报告中明确环境瓶颈。

## 3. 公平比较设计

### 3.1 结果分层

正式报告必须拆成四组，禁止合并成一个“HTTP/3 快多少”的数字：

1. **稳态反代主结果**：复用已建立 QUIC 连接和 stream，经相同 HTTP/1.1 backend；
2. **握手/短连接结果**：每连接一个请求，包含完整 TLS 1.3 + QUIC 握手成本；
3. **连接与 stream 扩展诊断**：改变连接数和每连接并发 stream，解释 worker/shard 落点；
4. **健壮性结果**：异常输入、网络扰动、过载、故障恢复和 soak，只判正确性与恢复，
   不参与性能排名。

主结果优先使用 lite-nginx 默认 `steal auto`。若 Phase 5 的前置 churn 失败，则仍完成
`steal off` 与 Nginx 的协议性能诊断，但报告标题、摘要和表格必须注明“默认配置未通过，
性能为回退配置结果”。

### 3.2 等价配置

| 项目 | lite-nginx | Nginx | 正式值 |
|---|---|---|---|
| worker/shard | 2 个 UDP endpoint | 2 workers + `reuseport` | 2 |
| QUIC version / ALPN | v1 / `h3` | v1 / `h3` | 相同 |
| TLS | BoringSSL | BoringSSL | TLS 1.3、相同证书 |
| Retry | 默认 off | `quic_retry off` | off |
| 0-RTT | 默认 off | `ssl_early_data off` | off |
| GSO | 编译时 `FIBER_ENABLE_UDP_GSO=OFF` | `quic_gso off` | off |
| bidi stream | 默认 128 | `http3_max_concurrent_streams 128` | 128 |
| per-stream 接收窗口 | 64 KiB | `http3_stream_buffer_size 64k` | 64 KiB |
| QPACK 请求编码 | 客户端可用静态表 | 客户端可用动态表 | 客户端强制动态表 0 |
| QPACK 响应编码 | 由客户端 SETTINGS 限制 | 由客户端 SETTINGS 限制 | 客户端广告容量 0 |
| access log | off | off | off |
| error log | error | error | error |
| 上游 | HTTP/1.1 | HTTP/1.1 | 同一 backend、keepalive |
| proxy buffering | off | off | off |

Nginx 的连接级初始接收窗口按 stream 数量和 64 KiB buffer 推导，而 lite-nginx 默认值更大。
正式请求并发和 body 大小必须低于两端 transport parameters 中的较小窗口；每次预检保存
双方实际 transport parameters。若测试确实触发连接级流控，应先暴露并对齐 lite-nginx
参数，然后重跑，不允许把不同窗口下的结果解释为实现性能差异。

### 3.3 单机 CPU 和进程隔离

延续前序测试的物理 core 避让，但把正式 SUT quota 降为 100%，给 HTTP/3 客户端、UDP
协议栈和公共上游留出更明确余量。两个 worker 仍保留，用于覆盖 `SO_REUSEPORT` 分片行为。

| 角色 | Linux vCPU | CPU quota | 说明 |
|---|---|---:|---|
| 被测服务 | `0,2,4` | 100% | master/accept 与 2 worker/shard 全部计入 |
| HTTP/3 load generator | `6,8,10,12` | 不额外限额 | 4 client workers |
| 公共 backend | `14,16` | 200% | 复用前序 benchmark backend |
| 监控、runner、UDP relay | `18` | 不额外限额 | 故障注入时才启 relay |
| 主测试不主动使用 | 所有奇数 vCPU | - | 避开 SMT sibling |

lite-nginx 和 Nginx 顺序运行，使用相同端口、AllowedCPUs、CPUQuota、MemoryMax 和文件
描述符上限；Nginx 必须 `daemon off`，确保 master/worker 都在同一 cgroup。正式矩阵不得
同时运行两个 SUT。为了连接分片可重复，每轮记录客户端源端口、server CID、客户端 worker
和实测每连接吞吐；不能只记录总 RPS。

可额外跑一组 200% quota 与前序 HTTP/2 数据做趋势衔接，但它是“bridge”数据，不与
100% quota 主表合并。

### 3.4 HTTP/3 压测客户端门槛

正式性能客户端必须基于独立于 lite-nginx 的 QUIC/HTTP/3 实现。候选方案优先使用固定
版本的 `h3load`，也可基于发行版 `ngtcp2` + `nghttp3` 构建仓库专用 driver；发行版当前
提供 `ngtcp2-client 0.12.1` 和 `libnghttp3-dev 0.8.0`，可无系统安装地下载并解包到
`temp/`。单请求互操作预检还要使用第二套不同的客户端栈：若性能客户端不是 ngtcp2，
则使用 `ngtcp2-client`；若性能 driver 已基于 ngtcp2，则改用固定版 quiche/curl HTTP/3
客户端。这样既不让被测端和客户端共享实现，也不让互操作结论只依赖一个客户端库。

选定客户端前必须通过能力探测：

- 强制 QUIC v1、ALPN `h3`、TLS 1.3，并输出协商结果和 transport parameters；
- 可配置连接数、每连接并发 stream、请求总数、持续时间和线程 affinity；
- 同时支持 closed-loop 吞吐和 open-loop 固定 RPS；
- GET、固定 64 KiB POST、响应长度/哈希校验；
- 每请求记录开始时间、首 header、响应 FIN、状态码、body bytes 和终止原因；
- 区分连接、TLS、QUIC transport、HTTP/3、QPACK、timeout 和校验错误；
- 测量截止后有独立 drain 时间，未完成 stream 不与真正错误重复计数；
- 输出每连接发送/接收 packet、丢包、重传、PTO、RTT、cwnd 和 key phase 统计；
- 能按 packet 数主动发起 key update；若不能，必须能按 packet 上限平滑轮换连接；
- 支持固定 QPACK 动态表容量为 0，保证两端请求和响应 header 编码策略可比。

不满足固定速率、逐请求延迟或 key-update 三项中的任何一项，都不能直接生成正式排名。
可以先用于互操作预检，但必须补齐 driver 或更换工具后再跑主矩阵。

### 3.5 密钥使用预算

每条连接维护当前 key phase 的 1-RTT 加密 packet 计数。正式 driver 在每代最多
1,000,000 个 packet 时主动发起 key update，远低于约 `2^23` 的 AES-GCM 保密性上限；
更新后要求两端继续成功处理请求，并验证旧 phase 的重排包在宽限期内可处理。

若客户端不能主动 update：

- 每条连接在 1,000,000 packet 前优雅关闭并错峰重连；
- 握手流量、重连次数单独计数，结果命名为 `connection-rotation mode`；
- 该模式不可与真正长连接 steady-state 结果直接比较；
- 2 小时长连接 soak 和 64 KiB 正式主结果暂停，不能越过包数预算继续跑。

## 4. 计划产物

执行时新增独立目录，不把 HTTP/3 逻辑塞入已有 HTTP/1/2 runner：

```text
scripts/benchmark/http3/
├── README.md
├── configs/
│   ├── lite_nginx_auto.conf
│   ├── lite_nginx_steal_off.conf
│   └── nginx_sut.conf
├── client/                       # 独立 QUIC/H3 driver 或固定工具适配层
├── payloads/
│   └── request_64k.bin
├── prepare_runtime.sh
├── verify_interop.sh
├── run_pair.sh
├── run_matrix.sh
├── run_latency_curve.sh
├── run_churn.sh
├── run_robustness.sh
├── run_soak.sh
├── udp_fault_relay.py            # 仅健壮性：有界丢包/重复/乱序/延迟
├── collect_cgroup.sh
└── summarize.py
```

若选用外部工具，其源码版本、构建参数和 SHA-256 必须固定；下载物、构建物和大型原始数据
只放 `temp/`，不提交仓库。计划和最终报告写入 `feature/`。

原始结果目录：

```text
temp/http3-benchmark-results/<timestamp>-<git-sha>/
├── environment/
├── interop/
├── capacity/
├── matrix/
├── latency/
├── churn/
├── faults/
└── soak/
```

每轮至少保存：Git commit/工作树状态、二进制和配置 SHA-256、Nginx `-V`、客户端版本，
完整命令，CPU/cgroup 参数，协商参数，逐请求日志，逐连接 QUIC 统计，SUT/client/backend
的 `cpu.stat`、`memory.current`、`memory.peak`、`memory.events`，进程状态和 error log，以及
运行前后 `/proc/net/snmp`、`nstat`、`ss -u -i` 的差值。

## 5. 正式场景矩阵

### 5.1 稳态反代主矩阵

| Case | 请求 | 连接 × stream | 主要观察 |
|---|---|---:|---|
| H3-P-1K | `GET /bench/1k` | `c8 × m64` | 小响应 RPS、协议固定成本 |
| H3-P-64K | `GET /bench/64k` | `c8 × m64` | goodput、拥塞/pacing、UDP 发送成本 |
| H3-P-POST | `POST /bench/echo`，64 KiB | `c8 × m64` | 双向流控、请求体内存、上游 echo |

`P` 表示 proxy。请求内容、响应长度、响应 SHA-256 和 status 必须与前序 HTTP/1/2
benchmark backend 完全一致。客户端在一个 request stream 完成后才把该并发槽用于下一
请求，不使用 0-RTT，不把握手阶段请求混入稳态测量。

每个实现每个场景 9 次配对运行：预热 20 秒、测量 60 秒、drain 最多 10 秒、冷却 30 秒。
奇数对 `lite -> nginx`，偶数对 `nginx -> lite`；同一对之间 backend 不重启。若 9 对后
RPS 配对变异系数仍大于 5%，增加到 13 对，不通过挑选“好看”的运行解决波动。

### 5.2 连接与 stream 诊断

固定 1 KiB GET，每点 3 次、预热 10 秒、测量 30 秒：

| Case | 参数 | 用途 |
|---|---:|---|
| H3-C1-M1 | `c1 × m1` | 单连接、单 stream 基线 |
| H3-C1-M64 | `c1 × m64` | 单连接内 stream 扩展 |
| H3-C2-M64 | `c2 × m64` | 两个 shard 的最低覆盖 |
| H3-C8-M64 | `c8 × m64` | 正式主参数 |
| H3-C32-M16 | `c32 × m16` | 更多连接下 reuseport 分布 |

若客户端可取得 server CID/connection stats，报告每连接 RPS；否则至少记录客户端源端口
并使用短时 server 诊断计数确认两个 shard 都有连接。出现单 shard 偏斜时该轮属于分片诊断，
不能用总 RPS 证明另一实现的 QUIC 核心更快。

### 5.3 握手和短连接

- Full handshake：每连接 1 个 1 KiB GET，Retry off，禁止 session resumption；
- Resumption：若两端和客户端均支持，单独测 1-RTT resumed handshake；
- Retry：单独启用后验证一次 Retry + token 流程，只作能力/成本诊断；
- 0-RTT：当前正式配置关闭。只有两端都能显式配置、能区分 replay-safe 请求且语义一致时，
  才增加独立结果，不作为首轮验收项。

指标为 connections/s、requests/s、handshake p50/p99、首字节和完整请求 p50/p99、失败率、
SUT CPU/connection 和 peak memory。不得把 full handshake 与 resumption 混在同一表中。

## 6. 执行阶段

### Phase 0：工具、环境和无副作用检查

- [ ] 保存 `uname`、`lscpu`、内存、WSL 版本、CPU governor/宿主机电源状态；
- [ ] 保存 UDP sysctl、`ulimit -n`、cgroup v2 和 systemd user unit 能力；
- [ ] 固定独立 HTTP/3 客户端版本并保存其完整 `--help`、构建参数和 SHA-256；
- [ ] 证明客户端满足 3.4 节的逐请求日志、fixed-RPS、QPACK=0 和 key-update 能力；
- [ ] 所有工具放在仓库 `temp/`，不替换系统库，不需要系统级安装；
- [ ] runner、故障 relay 和所有 case 带总超时及进程看门狗；
- [ ] 运行前后检查背景 CPU、UDP errors 和可用内存，不满足门槛则整轮作废。

### Phase 1：可比较构建

lite-nginx 使用独立 Release + LTO 构建，不使用 Debug 或 sanitizer 二进制做性能排名：

```bash
cmake -S . -B build-bench-h3 \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_BUILD_APPS=ON \
  -DFIBER_BUILD_EXAMPLES=ON \
  -DFIBER_BUILD_TESTS=OFF \
  -DFIBER_ENABLE_HTTP3=ON \
  -DFIBER_USE_JEMALLOC=OFF \
  -DFIBER_ENABLE_LTO=ON
cmake --build build-bench-h3 \
  --target fiber_app_lite_nginx http_benchmark_backend -j
```

Nginx 继续由 `scripts/build_nginx.sh` 构建。运行前同时检查：

```bash
build-bench-h3/apps/lite_nginx --config <lite-config> --check-config
temp/nginx-install/sbin/nginx -t -p "$PWD/" -c <nginx-config>
```

记录实际编译器、link line、BoringSSL 和 allocator；两个 SUT 均使用相同证书和 TLS 1.3。

### Phase 2：backend、配置和响应预检

- [ ] 启动既有 `http_benchmark_backend`，固定在 `127.0.0.1:19001`；
- [ ] 生成 HTTP/3 专用 lite auto/off 与 Nginx 配置，关闭 access log 和非等价功能；
- [ ] Nginx 明确写出 `http3 on`、`quic_retry off`、`quic_gso off`、128 streams、64 KiB buffer；
- [ ] 验证 UDP listener、ALPN `h3`、QUIC v1、TLS 1.3 和实际 transport parameters；
- [ ] 对三个 endpoint 校验 status、body length 和 SHA-256；
- [ ] 用第二个独立客户端至少完成 GET 1 KiB、GET 64 KiB、POST echo 64 KiB；
- [ ] 每次失败后再发一条新连接健康请求，区分请求错误和进程级故障。

### Phase 3：压测器与 backend 余量

1. 客户端直连 Nginx 静态 1 KiB 短路径，确认其四个 CPU 的平均使用率低于 65%，无 UDP
   drop、无 timeout，逐请求日志的总数和汇总完全一致；
2. backend 直压容量至少为预计 SUT 峰值的 2 倍，正式轮中 backend CPU 低于分配预算的
   50%；
3. SUT 在 100% quota 下能稳定触发 quota，但不能出现持续 cgroup throttling 导致的
   调度锯齿；若明显，应改用 AllowedCPUs=单 CPU 且 quota 不变，并对两端统一重跑；
4. 分别取 1 KiB 和 64 KiB 做 10/20/30 秒重复测试，确认客户端不是 packet-rate 或
   crypto 瓶颈。

任何一项不满足时，不开始正式矩阵。优先降低 SUT quota 或减少并发，而不是保留客户端
已经饱和的数据。

### Phase 4：互操作和协议正确性门槛

- [ ] 重复请求、并发 stream、请求 body、响应 body、trailers（支持时）和 graceful close；
- [ ] peer 发起 key update 后继续请求，并在宽限期内注入旧 phase 重排包；
- [ ] 客户端主动关闭、server CONNECTION_CLOSE、stream RESET/STOP_SENDING；
- [ ] Retry off 正常握手；Retry on 的独立 token 流程；
- [ ] server restart 后客户端能建立新连接，旧 CID 流量不会影响新连接；
- [ ] 两个 worker/shard 都实际处理连接；
- [ ] Nginx 同样执行，记录行为差异，但 Nginx 不是协议规范的替代品。

### Phase 5：正式性能前的健壮性烟测

对 lite-nginx `steal auto` 和 `steal off` 分别执行 20 轮：每轮新启动服务，8 connections、
每连接 64 streams、10 秒 1 KiB/64 KiB 混合负载，结束时客户端集中断连。每轮检查：

- 进程始终 active，无 assert、SIGABRT、core、hang 或 event-loop affinity 错误；
- 健康请求在 1 秒内恢复；
- 无遗留 UDP socket、连接或持续增长的 memory.current；
- client error 与 server close code 可解释且不重复计数。

`steal auto` 任一轮崩溃即判默认配置失败。后续只允许以 `steal off` 完成诊断数据，并在
报告中保留失败，不得通过重启或剔除该轮继续宣称默认配置稳定。

### Phase 6：稳定最大吞吐 Rmax

对三个主场景逐级增加 closed-loop 并发，先用 30 秒台阶发现拐点，再在候选点做 3 次
60 秒确认。稳定 Rmax 同时要求：

- 完成请求全部 status/body 校验正确；测量截止后的正常 drain 不算失败；
- 无 QUIC/H3/QPACK/server error、timeout、UDP buffer drop 或进程重启；
- p99 不连续三个窗口上升，后半程 RPS 不低于前半程 95%；
- client CPU 低于 65%，backend CPU 低于 50%；
- SUT memory 已进入平台，没有持续线性增长。

Phase 6 只用于选主矩阵并发和 fixed-RPS 档位，不作为最终排名。

### Phase 7：9 对正式稳态矩阵

按 5.1 节完成平衡配对运行。所有原始运行都进入汇总，包括异常轮；只有明确的外部噪声
（例如宿主机更新、非 SUT 进程抢占、UDP 系统 drop）可按预先定义规则标记 invalid，且必须
在报告中列出原因和重跑编号。

汇总使用每对 lite/Nginx 比值的中位数，并用固定随机种子的 paired bootstrap 输出 95% CI。
同时报告 9 个原始点、运行顺序和 connection 分布，避免总表隐藏双峰。

### Phase 8：固定速率延迟曲线

对 1 KiB 和 64 KiB 两个代表场景，以两端较小稳定 Rmax 为共同基准，施加 30%、50%、
70%、85%、95% 五档相同绝对 offered RPS。每档每方 3 次、每次预热 30 秒、测量 5 分钟、
drain 10 秒。

报告 achieved/offered RPS、p50/p90/p99/p99.9、排队延迟、timeout、重传、PTO、CPU 和内存。
若客户端不支持 coordinated omission 修正或 open-loop 调度，该阶段不得用 closed-loop
延迟冒充 fixed-RPS 延迟。

### Phase 9：健壮性、故障恢复和 soak

#### 9.1 连接和 stream 生命周期

- 至少 100,000 次 full handshake churn，连接创建后 0/1/多请求随机关闭；
- 连接仍有 1、64、128 个 active stream 时客户端进程退出或集中断开；
- 随机 10% request stream 发送 RESET，随机 10% 响应中途 STOP_SENDING；
- 请求 body 发送一半后 FIN、RESET 或停止发送；响应客户端停止读取；
- 连接空闲超时、graceful shutdown、server restart 和立即 restart；
- 每 20 个 case 发独立健康请求并记录恢复时间。

#### 9.2 有界网络扰动

通过用户态 UDP relay，仅对当前 case 注入，不修改系统 `tc`：

- 0.1%、1%、3% 随机丢包；
- 1%、5% packet duplication；
- 10/50 ms 有界乱序和延迟；
- 丢弃大于 1200/1280 字节的 datagram，验证 MTU/重传行为；
- client source port rebinding，单独观察 path validation/migration。

这些 case 只要求正确完成或按协议关闭、进程存活并恢复，不比较吞吐。relay 自身 CPU 饱和
或队列溢出的 case 作废。

#### 9.3 QUIC/HTTP/3 边界输入

使用独立协议 driver 生成可认证的边界输入，而不是随机修改密文：

- 小于 1200 字节 Initial、unsupported version、坏 token、重复 transport parameter；
- stream/data/connection flow-control 超限、stream limit 超限、final size 冲突；
- 非法 frame type、截断 varint、保留位、错误 encryption level 上的合法 frame；
- 缺失/重复 SETTINGS，SETTINGS 位于错误 stream，重复 critical uni stream；
- DATA before HEADERS、request stream 上的禁止 frame、重复/乱序 pseudo-header；
- Content-Length 与 DATA 长度不符、trailers 后再次 DATA/HEADERS；
- QPACK 非法 index/instruction、超过已广告容量、blocked stream 边界；
- 关闭 control/QPACK critical stream 后，验证连接级错误和独立新连接健康。

每个 case 预定义期望的 QUIC 或 HTTP/3 close code；Nginx 同跑并记录差异，但最终正确性以
协议期望为准。所有 case 有每秒 datagram 上限、最大 256 并发、`MemoryMax=1GiB` 和总超时。

#### 9.4 上游故障与过载恢复

- backend 延迟、接收后关闭、部分响应后关闭、挂起、停止和重启；
- 2×、5× Rmax 过载各 30 秒，随后降到 50% Rmax；
- 过载中建立新 QUIC 连接和独立健康请求，检查是否饿死；
- backend 恢复后记录首个成功请求时间和吞吐恢复曲线；
- `steal auto/off` 分开记录，不能用 off 掩盖 auto 的生命周期错误。

过载结束 10 秒内吞吐应恢复到过载前同档位的 90%，60 秒内 memory.current 应回到稳定
平台；若 allocator 保留内存，只要求不继续单调增长，并在报告中给出 peak/plateau。

#### 9.5 两小时 soak

以两端共同 Rmax 的 70% 运行 2 小时，1 KiB/64 KiB/POST 按 70%/20%/10% 混合，连接和
stream 持续复用，并至少跨越多次主动 key update。每 10 秒采样 CPU、memory、请求、packet、
RTT、重传、PTO、active connections/streams 和 UDP errors。

通过条件：零错误、零进程退出、健康探测连续成功；去掉前 15 分钟后，memory.current
每小时增长小于 1 MiB，最后 15 分钟中位数不超过首个稳定 15 分钟中位数的 105%。
lite-nginx 默认配置和 Nginx 各跑一次；若默认 auto 已在 Phase 5 失败，不执行伪装成通过项
的 auto soak，只跑 off 用于定位，并保留总体失败结论。

### Phase 10：sanitizer 回归

ASan+UBSan 使用较低并发重跑互操作、churn、stream reset、协议边界和上游故障；TSan 只跑
最容易触发跨 event-loop/连接池竞态的集中断连子集。sanitizer 构建不参与性能排名，任何
报告都保存完整堆栈和可复现 seed。

## 7. 指标和统计口径

### 7.1 性能指标

- 成功 RPS、响应 goodput MiB/s；
- request p50/p90/p99/p99.9/max，握手和 TTFB 单独统计；
- SUT CPU seconds/Mreq、requests/CPU-second、CPU seconds/GiB；
- client/backend CPU headroom；
- memory.current、memory.peak、稳定平台和每连接/stream 增量；
- connections/s、每连接 RPS、worker/shard 分布；
- packets/request、retransmitted packets/bytes、PTO、RTT、cwnd、key updates；
- UDP `InErrors`、`RcvbufErrors`、`SndbufErrors`、kernel drop；
- QUIC transport、TLS、HTTP/3、QPACK、timeout、body 校验错误分别计数。

延迟从客户端“请求允许发送”到响应 FIN；open-loop 同时保存计划发送时间，单独报告排队
延迟。测量结束仍在 drain 的 stream 与真正 failed/errored/timeout 不重复相加。

### 7.2 差异判定

- 主值为 9 个配对比值的中位数，给出 paired bootstrap 95% CI；
- RPS 差异需同时满足 CI 不跨 1.0、绝对差至少 5%、正确性门槛通过，才表述为明确更快；
- p99 需结合相同 offered RPS 比较，饱和点不同的 closed-loop p99 不直接排名；
- 若连接分片双峰、client/backend 瓶颈或 UDP drop 出现，只报告诊断，不下实现优劣结论；
- Nginx 的可选 `quic_gso on` 结果只与其自身 off 比较，不能进入等价 lite/Nginx ratio。

## 8. 验收标准

### 8.1 正确性和互操作

- 两个独立客户端均能与双方完成 QUIC v1 + `h3` 请求；
- 三个主 endpoint 的 status、长度和 SHA-256 全部正确；
- 主矩阵零协议错误、零 timeout、零 body mismatch、零 UDP drop；
- peer key update 和至少两次连续 key phase 轮换成功；
- transport parameters、QPACK 和流控差异已被控制或明确标记。

### 8.2 健壮性

- Phase 5 的 20/20 churn 无 crash、assert、core、hang；
- 2 小时 soak 满足零错误和内存平台标准；
- 协议边界 case 返回预期层级的 close/error，独立连接健康不受影响；
- 丢包、乱序、RESET、上游故障和过载后在规定时间恢复；
- ASan/UBSan 无错误，TSan 子集无数据竞争；
- 主动 key update 不具备时，长连接 64 KiB 性能和 soak 明确为“未执行/阻塞”，不能算通过。

### 8.3 性能结果有效性

- SUT、客户端、backend 和 UDP 内核路径均满足余量门槛；
- 正式场景至少 9 个有效配对，顺序平衡且无选择性剔除；
- 配置、二进制、命令、原始日志和统计脚本可复现；
- 报告同时给绝对值、配对比值、CI、失败数和资源开销；
- 默认 `steal auto` 若失败，报告不得只展示 `steal off` 排名而省略默认失败。

## 9. 推荐执行顺序和停止规则

1. 完成工具能力探测、独立客户端互操作和 key-update 前置门槛；
2. 完成 Release 构建、等价配置、响应校验、客户端/backend 余量；
3. 先跑 20 轮 auto/off churn。出现进程级失败时立即保存现场，默认配置判失败；
4. 仍可用 off 完成协议性能诊断，但先跑 Rmax discovery，再跑 9 对正式矩阵；
5. 完成连接分片、握手和 fixed-RPS 曲线；
6. 完成协议边界、网络扰动、上游故障、过载恢复；
7. 所有短时门槛通过后再跑 2 小时 soak 和 sanitizer 子集；
8. 汇总原始数据，生成新的 HTTP/3 压测报告，不覆盖前序 HTTP/1/2 报告。

预计独占机器时间为 12 至 18 小时，不含客户端 driver 的实现时间。任何阶段出现 crash、
协议数据错误、客户端饱和、backend 饱和、UDP drop 或 key 使用预算失控，都先停止相应
正式阶段并报告原因；不能用延长测试或重复挑选运行来掩盖前置门槛失败。

## 10. 最终报告结构

执行报告至少包含：

1. 一页结论：默认配置是否通过、lite/Nginx 主场景差异、最严重的健壮性发现；
2. 环境、构建、工具版本、CPU/cgroup、UDP sysctl 和协议参数；
3. 9 对稳态性能表及 paired bootstrap CI；
4. fixed-RPS 延迟曲线、握手和连接/shard 扩展表；
5. CPU、内存、packet/retransmission/key-update 和 UDP drop 数据；
6. churn、协议边界、网络扰动、过载、上游故障和 soak 结果；
7. sanitizer 结果、失败现场、复现命令和已知未执行项；
8. 结论边界与优化优先级。

报告必须把“互操作通过”“短时正常负载稳定”“异常输入可恢复”“2 小时 soak 通过”和
“性能更优”作为五个独立结论，不能用其中一项替代其他项。
