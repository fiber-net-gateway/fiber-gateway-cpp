# lite-nginx HTTP/3 压测问题根因与修复

日期：2026-07-20  
关联计划：[lite-nginx 与 Nginx 单机 HTTP/3 性能及健壮性压测计划](lite_nginx_nginx_single_host_http3_benchmark_plan.md)  
原始报告：[lite-nginx 与 Nginx 单机 HTTP/3 压测报告](lite_nginx_nginx_single_host_http3_benchmark_report.md)

## 1. 结论

本轮针对原始 HTTP/3 压测暴露的问题完成了代码检查、Nginx 对照、修复和小规模回归。
结论需要按问题归属分别判断：

| 问题 | 原因 | 处理结果 |
|---|---|---|
| lite-nginx 64 KiB POST echo 返回 `502 Bad Gateway`，日志为 `phase=send_body error=already` | HTTP/3 请求体数据和 FIN 可在两次读取中到达；HTTP/1.1 上游在收到完整 `Content-Length` 数据后已自动结束，代理却又把后续空 FIN 标记写入已结束 exchange | **已修复**；精确长度和 SHA-256 校验通过，运行日志不再出现该错误 |
| 64 KiB GET 吞吐低、CPU 效率差 | QUIC pacer 最小粒度为 1 ms，事件循环也只能以整数毫秒等待，短 RTT loopback 下的发送调度被粗粒度定时放大 | **已修复主要瓶颈**；lite 中位 RPS 从 289 提升至 1,141，约 3.95 倍 |
| 1 KiB GET p99 约 30 ms，吞吐仍明显低于 Nginx | 仍与用户态 pacing/小响应发包调度相关；关闭 pacing 的单次诊断明显改善 1 KiB，但 64 KiB 诊断没有完成请求，不能安全地全局关闭 | **部分改善、未完全解决**；保留 pacing，避免用未经验证的配置掩盖问题 |
| 连续两次 key update 失败 | patched ngtcp2 客户端在第一次更新尚未完成可再次更新的状态转换时立刻发起第二次，客户端自身返回 `ERR_INVALID_STATE` | **不修改服务端**；需要修复测试驱动的确认时序后重测 |
| NAT rebinding 不稳定 | lite-nginx 和 Nginx 均出现间歇失败，本轮客户端最终为 `ERR_IDLE_CLOSE`，现有结果不能区分客户端时序和服务端 path validation | **证据不足，不做猜测性修复**；需要确定性迁移驱动和 qlog/path trace |
| 高并发出现 UDP `RcvbufErrors` | 单机内核 `net.core.rmem_max=212992`，而且 Nginx 高 packet rate 下同样丢包 | **环境限制，不修改协议代码**；正式对比仍使用双方共同零丢包的 `c1×m1` 点 |

这里的健壮性检查仅用于开发程序的协议互操作和受控压力回归，不是安全测试。

## 2. POST 64 KiB `502/already`

### 2.1 复现链路

请求带有 `Content-Length: 65536` 时，HTTP/3 DATA payload 恰好填满代理一次 64 KiB
读取。`ServerHttp3Request::read_body()` 会立即返回这 65536 字节；如果 QUIC stream FIN
尚未进入当前输入缓冲，这个非空 chunk 不带 `complete`。下一次读取解析到 FIN 后，再返回
一个 `readable_bytes()==0 && complete()==true` 的空完成标记。

旧代理循环把两次结果都原样写给 HTTP/1.1 上游：

1. 第一次写入 65536 字节；
2. `ClientHttp1Exchange::write_body()` 发现已达到声明的 `Content-Length`，自动把
   `request_state_` 切换为 `RequestDone`；
3. 第二次代理再写入空完成标记；
4. HTTP/1.1 exchange 因已是 `RequestDone` 返回 `IoErr::Already`，代理映射成 502。

这不是 POST body 内容损坏，也不是 QUIC 连接崩溃，而是不同协议的“完成”语义在代理边界
上重复提交。

### 2.2 Nginx 对照

仓库固定的 Nginx 1.31.3 源码采用两个独立状态来处理这个边界：

- `src/http/v3/ngx_http_v3_request.c:1731-1764` 在 H3 FIN 到达时校验实际 body 长度，
  再产生一个 `last_buf`；
- `src/http/ngx_http_upstream.c:2280-2395` 的非缓冲 upstream 循环分别观察
  `r->reading_body` 和待发送 chain；读取结束且没有输出后停止，不会把完成事件再次写进
  已结束的 HTTP/1.1 请求。

Nginx 在这里作为实现对照，不代表规范要求项目复制其内部结构；对照说明代理必须在协议
边界显式持有请求体转发状态。

### 2.3 修复

在 `src/http/HttpProxyCore.h` 增加无动态分配的 `RequestBodyForwardState`，并在
`apps/lite_nginx/src/proxy/ProxyHandler.cpp` 的请求体转发循环中使用：

- 已知 `Content-Length` 时跟踪尚未转发的字节数；
- 拒绝超过声明长度的数据，异常时同时终止上下游 exchange；
- 声明长度已经写满后，忽略随后到达的空完成标记；
- `Content-Length: 0` 且 header 尚未带 end-stream 时，仍把第一个空完成标记提交给上游；
- chunked/未知长度请求仍转发空完成标记，以保留 HTTP/1.1 chunk terminator 语义。

这个状态对象只包含一个布尔值和一个字节计数，不在热路径引入 `std::string`、容器或额外
堆分配。

### 2.4 验证

新增 `tests/HttpProxyCoreTest.cpp`，覆盖：

- 完整 `Content-Length` 后跳过延迟到达的空完成标记；
- 拒绝超过声明长度的后续数据；
- 零长度请求仍提交第一次完成标记；
- chunked 请求仍转发空完成标记。

实际 HTTP/3 回归的 POST response 为 65536 bytes，SHA-256 为：

```text
de2f256064a0af797747c2b97505dc0b9f3df0de4f489eac731c23ae9ca9cc31
```

协议回归目录 `protocol-after-fixes-final-20260720` 中 `post-echo-64k=pass`，服务日志中没有
`phase=send_body`、`already` 或 502。

## 3. QUIC 定时与 64 KiB GET 性能

### 3.1 根因

原实现存在两层低分辨率限制：

1. `QuicPacingOptions::timer_granularity` 默认为 1000 us，短 RTT 下所有亚毫秒 pacing
   deadline 被向上取整到 1 ms；
2. `EventLoop` 把下一 timer deadline 截断成整数毫秒，`Poller` 只能调用
   `epoll_wait(..., timeout_ms)`。

在 loopback 的亚毫秒 RTT 和 WSL2 调度环境中，整数毫秒 timer 会把发送预算补充切成较粗
的突发，增加尾延迟和无效调度。原始报告中 64 KiB GET 的 lite CPU 利用率达到 87.4%，
但只有 289 RPS 和 330 req/CPU-s，符合发送调度成本过高的表现。

固定 Nginx 的 QUIC 输出路径没有同样的独立用户态 pacer：

- `src/event/quic/ngx_event_quic_output.c:71-110,115-195` 在 congestion window 内持续组装并
  发送 datagram，只为丢包、idle 和 socket retry 等事件设置 timer；
- `src/event/quic/ngx_event_quic.c:315-320` 初始化 congestion window。

这解释了两者调度模型不同，但不等于可以直接关闭 lite-nginx 的 pacing。

### 3.2 修复

- `EventLoop` 和 `Poller` 的 timeout 接口改为 `std::chrono::nanoseconds`；
- Linux 上优先使用 `epoll_pwait2`，把 deadline 以 `timespec` 原样交给内核；
- 旧内核返回 `ENOSYS` 时退回 `epoll_wait`，并对毫秒 timeout 向上取整，避免提前唤醒形成
  busy loop；
- QUIC pacer 默认最小粒度由 1000 us 调整为 100 us；
- 新增单元测试，确认默认 pacer 在 1 ms RTT 下可以生成 `start + 100 us` deadline。

### 3.3 修复验证结果

为快速验证修复，使用同一台主机、GSO off、QPACK=0、SUT 单 CPU affinity、`c1×m1`，
每点 3 次、每次 5 秒。样本数小于原报告的 9 对正式矩阵，因此本表是修复回归，不替代原
报告的统计结论。

| Case | 原始 lite RPS | 修复后 lite RPS | 变化 | 修复后 Nginx RPS | 修复后 lite/nginx |
|---|---:|---:|---:|---:|---:|
| H3 GET 1 KiB | 485 | 457 | 0.94× | 5,799 | 0.079 |
| H3 GET 64 KiB | 289 | 1,141 | **3.95×** | 6,241 | 0.173 |
| H3 POST 64 KiB | 不可测（正确性失败） | 1,031 | 新增有效结果 | 864 | 1.193 |

其他观察：

- GET 64 KiB 的 lite p99 从原报告 5.40 ms 降至本轮 2.62 ms；
- 独立的三次 lite-only 验证中，GET 64 KiB 为 1,192 RPS、p99 2.15 ms、
  3,272 req/CPU-s，说明最终配对矩阵中的一次 251 RPS 是明显波动点；
- POST 64 KiB 的三次正确性 gate 和 timed load 均成功，无 5xx、timeout 或 UDP drop；
- POST 的 3 个短样本中 lite RPS 高于 Nginx，但样本不足，不能据此宣称总体性能领先。

最终配对矩阵 18 个运行均无 gate/load failure、HTTP 5xx、请求失败或 UDP error counter
增量。每个 lite timed run 的 `status_zero` 是截止时仍在途的单个 stream，不计入成功 RPS，
也不是 HTTP 错误。

## 4. 仍需后续处理的边界

### 4.1 1 KiB 小响应 pacing

高分辨率 timer 没有消除约 30 ms 的 p99 周期性尾延迟。一次仅关闭 lite pacing 的诊断中，
1 KiB 达到 2,958 RPS、p99 1.06 ms，证明 pacing/发包调度仍是主要方向；但相同改动下的
64 KiB 诊断没有形成完成请求，因此本轮已撤回全局关闭 pacing 的实验改动。

后续应先增加 connection 级 trace，记录每次 pacer budget、目标 deadline、实际唤醒时间、
生成的 packet 数和 response header/body 是否分包，再评估以下方案：

1. 小响应 header/body 在同一 event-loop turn 内合并组包；
2. 将用户态 pacing 替换为经过验证的 kernel pacing/`SO_TXTIME` 路径；
3. 仅在有明确拥塞窗口和 burst 上限保护时允许小响应受控 burst。

验收必须同时覆盖 1 KiB、64 KiB、丢包和多连接，不接受只提高单一 microbenchmark 的修改。

### 4.2 连续 key update

最终日志显示客户端先输出两次 `Initiate key update`，第二次调用立即在客户端报：

```text
ngtcp2_conn_initiate_key_update: ERR_INVALID_STATE
```

lite-nginx 和 Nginx 都出现同一失败点，因此当前 case 没有验证到“服务端处理第二个新 key
phase”。测试驱动需要在第一次更新后等待：新 key phase 的 packet 被发送、收到 peer 对该
phase 的 packet/ACK，并且 ngtcp2 重新允许主动更新，然后才能发起第二次。修复驱动前不应
修改服务端 key update 状态机。

### 4.3 NAT rebinding

原报告四次运行中 lite 通过 1/4、Nginx 通过 2/4；最终 lite 运行在客户端报
`ngtcp2_conn_handle_expiry: ERR_IDLE_CLOSE`。这不是足以定位服务端 path validation 的证据。
后续客户端应明确记录 socket 切换前后的地址、PATH_CHALLENGE/PATH_RESPONSE、活动 path 和
ACK，并至少重复 20 次；双方使用同一迁移时序和判定条件。

### 4.4 单机 UDP buffer

本轮不修改 host sysctl。需要做高并发容量测试时，应在用户明确允许后把
`net.core.rmem_max`、`net.core.wmem_max` 和 socket buffer 作为受控实验变量，并同时记录
`UdpInErrors/RcvbufErrors/SndbufErrors`。在当前 212,992-byte 上限下，带 drop 的高并发点
不能用于 lite-nginx/Nginx 排名。

## 5. 最终健壮性回归

协议检查结果：

| Case | 结果 |
|---|---|
| GET 1 KiB 精确 hash | 通过 |
| curl HTTP/3 GET 1 KiB | 通过 |
| GET 64 KiB 精确 hash | 通过 |
| POST echo 64 KiB 精确 hash | **通过** |
| 1% client TX/RX loss | 通过 |
| 120 个有界随机/截断 UDP datagram 后健康检查 | 通过 |
| 连续两次 key update | 客户端驱动未到达第二次有效更新 |
| NAT rebinding | 不稳定，当前证据不足 |

另执行 5 轮进程重启和 3 秒 HTTP/3 churn：5/5 load 与结束后健康检查成功，约 10.8 万个
已完成请求全部成功，进程始终 active，MemoryPeak 为 47,943,680–48,676,864 bytes；没有
assert、crash、hang、502 或 `already`。

最终 `cmake --build build -j4` 成功；
`ctest --test-dir build --output-on-failure` 为 1392/1392 通过，两个显式启用才运行的 r-nacos
互操作测试处于 skipped，0 failed。

## 6. 变更与复现

代码和测试：

```text
apps/lite_nginx/src/proxy/ProxyHandler.cpp  请求体转发边界状态
src/http/HttpProxyCore.h                   RequestBodyForwardState
src/event/EventLoop.{h,cpp}                纳秒级 timer deadline
src/event/Poller.{h,cpp}                   epoll_pwait2 与兼容回退
src/quic/QuicPacer.h                       100 us 默认粒度
tests/HttpProxyCoreTest.cpp                POST 边界单元测试
tests/QuicPacerTest.cpp                    亚毫秒 pacing 单元测试
```

本轮关键复现命令：

```bash
# 精确响应、丢包、迁移、key update 和有界 datagram
IMPLEMENTATIONS=lite SUT_CPUS=0 SUT_QUOTA=none \
RUN_ID=protocol-after-fixes-final-20260720 \
  scripts/benchmark/http3/run_protocol_checks.sh

# 修复后的 lite/Nginx 小规模配对矩阵
H3_CLIENTS=1 H3_STREAMS=1 LOAD_THREADS=1 \
IMPLEMENTATIONS='lite nginx' REPETITIONS=3 DURATION=5 WARMUP=1 COOLDOWN=1 \
SUT_CPUS=0 SUT_QUOTA=none RUN_ID=performance-after-fixes-final-20260720 \
  scripts/benchmark/http3/run_matrix.sh

# 短连接 churn 回归
IMPLEMENTATIONS=lite ROUNDS=5 DURATION=3 SUT_CPUS=0 SUT_QUOTA=none \
RUN_ID=churn-after-fixes-final-20260720 \
  scripts/benchmark/http3/run_churn.sh
```

原始结果位于：

```text
temp/http3-benchmark-results/protocol-after-fixes-final-20260720/
temp/http3-benchmark-results/performance-after-timer-fix-20260720/
temp/http3-benchmark-results/performance-after-fixes-final-20260720/
temp/http3-benchmark-results/performance-pacing-off-diagnostic-20260720/
temp/http3-benchmark-results/performance-pacing-off-64k-diagnostic-20260720/
temp/http3-benchmark-results/churn-after-fixes-final-20260720/
```
