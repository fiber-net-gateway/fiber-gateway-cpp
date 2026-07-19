# lite-nginx 与 Nginx 单机 HTTP/3 压测报告

测试日期：2026-07-19  
代码版本：`6ea42a68c492284f111fb8d98e55ac50237769f3`（压测设施为未提交工作树改动）  
执行计划：[lite-nginx 与 Nginx 单机 HTTP/3 性能及健壮性压测计划](lite_nginx_nginx_single_host_http3_benchmark_plan.md)

## 1. 结论摘要

本轮完成了 HTTP/3 独立客户端构建、Release/LTO 双版本构建、等价配置、响应哈希校验、
单 CPU 亲和的 9 对 GET 诊断矩阵、GSO 对照、20+20 轮连接 churn，以及丢包、NAT
rebinding 和有界异常 UDP datagram 的短时检查。最终有效 GET 矩阵包含 36 个运行、
1,244,544 个 2xx 响应。

五项结论必须分开看：

1. **互操作未整体通过。** lite-nginx 与 Nginx 均能以 QUIC v1、TLS 1.3、ALPN `h3`
   完成 1 KiB/64 KiB GET，且 bsslclient 和 curl 的响应哈希正确；但 lite-nginx 的
   64 KiB POST echo 稳定返回 `502 Bad Gateway`，Nginx 返回正确的 64 KiB body。
2. **短时 GET 和连接 churn 稳定。** `steal auto` 与 `steal off` 各 20 轮、共 40/40
   轮保持 active，负载和结束后健康检查均成功，无 assert、SIGABRT、core 或 hang。
3. **只通过了有界健壮性子集。** 双方均通过 1% client TX/RX loss 和 120 个随机/截断
   loopback UDP datagram 后的健康检查。NAT rebinding 多次运行不稳定；连续两次 key
   update 被客户端驱动的 `ERR_INVALID_STATE` 阻断，不能算 SUT 通过或失败。
4. **2 小时 soak、fixed-RPS 曲线和 sanitizer 未执行。** POST 正确性、连续 key update
   和第二套独立 QUIC 栈的前置门槛未满足，按计划停止规则不能把后续阶段写成通过。
5. **性能只形成零丢包串行诊断，不形成完整正式排名。** 在双方相同 `c1×m1`、QPACK=0、
   GSO off、单 CPU 亲和条件下，lite/nginx 配对 RPS 中位比为：1 KiB `0.0548`
   （95% CI `0.0507–0.1616`），64 KiB `0.0493`（`0.0461–0.0608`）。该点显示 Nginx
   明显更快，但它是本机低 UDP buffer 下的共同零丢包点，并非两端各自稳定 Rmax。

最优先修复项是 HTTP/3 POST request body 向 HTTP/1.1 上游转发时的 `IoErr::Already`。
修复后应先重跑精确 echo、连续 key update 和 20 轮 churn，再恢复 POST、fixed-RPS 与 soak。

## 2. 环境、构建和工具

### 2.1 主机与隔离

- WSL2 Linux `6.6.114.1-microsoft-standard-WSL2`；
- Intel Core i7-13700H，WSL2 可见 20 个逻辑 CPU、10 个 core、2 threads/core；
- cgroup v2、user systemd transient units；
- SUT：`CPUAffinity=0`，无 CFS quota，两个配置 worker 共享一个逻辑 CPU；
- load generator：`taskset` 到 `6,8,10,12`，最终串行矩阵使用 1 个 load thread；
- backend：`CPUAffinity=14,16`、`CPUQuota=200%`；
- `net.core.rmem_max=212992`、`net.core.wmem_max=212992`、
  `net.core.netdev_max_backlog=1000`；测试未修改 sysctl。

计划原先使用 `AllowedCPUs` 和 100% quota。实测 user slice 没有委派 cpuset controller，
`AllowedCPUs=0` 不产生有效 cpuset；100% CFS quota 又在 10 秒测量中造成约 8 秒
throttling。最终改为 systemd `CPUAffinity=0`，原始 unit 快照中的 `taskset -pc` 确认
主进程 affinity 为 CPU 0，SUT `throttled_usec` 增量为 0。

### 2.2 二进制

| 组件 | 版本/构建 | SHA-256 |
|---|---|---|
| lite-nginx GSO off | Release + LTO、Clang 22、glibc allocator | `ec2cb767a87c4d879556d9b7e1906c412dea067d572e1c39cd3d5a4bb0338679` |
| lite-nginx GSO on | Release + LTO、Clang 22 | `766e09a0ebbb218314edece8d296821b4cee0e3ea9f16f9bb12dd05864990255` |
| Nginx | 仓库固定 1.31.3、Clang 22、BoringSSL | `df34817db9620748ca5f505e6ca4b96f87bad87d568b6fccd869221ad7159dfb` |
| benchmark backend | Release + LTO | `0b0c1994c5d148c03f5dc6a72197dab854010cb405d7636dfb6e76742b8807ee` |
| h2load | nghttp2 1.69.0、ngtcp2 1.24.0、nghttp3 1.17.0、BoringSSL | `96dfb468d99a8f76475fc3718ada4a31b3e26c8bb2d2422ea554f73e9d62743a` |

外部源码固定提交：nghttp2 `68cb6900fde14c77f0cd7add0e094a862960eb99`、ngtcp2
`956e7b4cfb6762312434ecdddb796baf0775f12a`、nghttp3
`06b46ec9189be3a49c78a283b625882d8a6e3237`。协议检查还使用 snap curl 8.21.0
（ngtcp2 1.23.0、nghttp3 1.17.0）。curl 与 h2load 是不同应用和依赖版本，但都基于
ngtcp2，因此不满足计划要求的“第二套独立 QUIC 实现”。

### 2.3 等价配置与源码核对

双方均为 2 worker/shard、相同证书和 TLS 1.3、Retry off、0-RTT off、128 bidi streams、
64 KiB stream buffer、HTTP/1.1 keepalive backend、access log off、proxy buffering off。
主表双方和客户端都关闭 GSO，并显式设置 h2load
`--header-table-size=0 --encoder-header-table-size=0`。

仓库固定 Nginx 源码确认：

- `temp/nginx-1.31.3/src/http/v3/ngx_http_v3_module.c:233-245` 将并发 stream 默认设为
  128、stream buffer 设为 65536，Retry/GSO 默认关闭；
- `temp/nginx-1.31.3/src/http/v3/ngx_http_v3_module.c:201,236` 与
  `temp/nginx-1.31.3/src/http/v3/ngx_http_v3.h:46` 表明其默认 QPACK table 为 4096、blocked streams
  跟随并发 stream；客户端广告 0 后响应动态表被限制为 0。

lite-nginx 的 `src/http/Http3Server.cpp:130-154,249-253` 确认每个 worker 创建一个
reuseport UDP shard；`src/http/Http3Protocol.h:31-36` 的 QPACK table/blocked streams
默认均为 0。

## 3. 正确性和互操作

### 3.1 精确响应

| Case | lite-nginx | Nginx | 校验方式 |
|---|---|---|---|
| GET `/bench/1k` | 通过 | 通过 | 1024 bytes + SHA-256 |
| curl HTTP/3 GET 1 KiB | 通过 | 通过 | 第二应用客户端 + SHA-256 |
| GET `/bench/64k` | 通过 | 通过 | 65536 bytes + SHA-256 |
| POST `/bench/echo` 64 KiB | **失败：502/16 bytes** | 通过 | 响应长度 + SHA-256 |

期望 POST hash 为
`de2f256064a0af797747c2b97505dc0b9f3df0de4f489eac731c23ae9ca9cc31`。
lite 实际 body 是 16 字节 `502 Bad Gateway\n`，服务日志为：

```text
phase=send_body error=already
```

进程仍为 `ActiveState=active`、`Result=success`。错误记录来自
`apps/lite_nginx/src/proxy/ProxyHandler.cpp:222-235` 的请求 body 读取/上游写入循环，
说明是 H3 request body 转发状态错误，而不是进程级崩溃。

### 3.2 协议与扰动子集

| Case | lite-nginx | Nginx | 结论 |
|---|---|---|---|
| QUIC v1 + TLS 1.3 + ALPN `h3` | 通过 | 通过 | h2load 协商输出确认 |
| 1% client TX/RX loss，10×64 KiB | 通过 | 通过 | 请求完成、进程存活 |
| 120 个 0–1199 bytes 随机/截断 datagram 后健康检查 | 通过 | 通过 | 独立新连接 1 KiB GET 成功 |
| NAT rebinding | 最新轮失败 | 最新轮通过 | 四次执行中 lite 1/4、Nginx 2/4；时序/客户端表现不稳定，结论不充分 |
| 连续两次 client key update | 未通过门槛 | 未通过门槛 | 双方第二次均由客户端返回 `ERR_INVALID_STATE`，不能归因于服务端 |

这里的随机 datagram 仅发送到本机当前测试端口，总计 120 个，有固定长度集合和总量；
它是协议健壮性检查，不是安全扫描。为了验证连续更新，临时 bsslclient 已把一次性 timer
改为周期 timer，并在更新后触发发送；第二次调用仍在双方测试中被 ngtcp2 客户端拒绝。

## 4. 零丢包串行性能诊断

### 4.1 方法和有效性

- `c1×m1`，1 个 h2load thread；双方相同连接/stream 数；
- 每个 GET case 9 对，奇数 `lite -> nginx`、偶数反序；
- 每轮预热 2 秒、测量 10 秒、冷却 1 秒；
- SUT 单 CPU affinity、QPACK=0、GSO off；
- 主指标为逐请求 TSV 中 2xx 数/10 秒，不使用 h2load 在截止时混合 warmup 的总汇总；
- 9 对均纳入 fixed-seed paired bootstrap；
- 36/36 gate/load 成功，HTTP 4xx/5xx、timeout、cgroup throttle 和三个 UDP error
  counter 增量均为 0。

选择 `c1×m1` 不是因为它代表最大吞吐，而是因为本机 212,992-byte UDP buffer 在更高
Nginx packet rate 下出现 `RcvbufErrors`。因此本节可比较串行请求完成能力、CPU 成本和
延迟，不可解释为双方各自 Rmax。

### 4.2 结果

| Case | lite RPS | nginx RPS | lite/nginx RPS ratio (95% CI) | lite p99 ms | nginx p99 ms | p99 ratio | lite req/CPU-s | nginx req/CPU-s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| H3 GET 1 KiB | 485 | 8,839 | 0.0548 (0.0507–0.1616) | 29.93 | 0.414 | 72.28 | 5,402 | 16,862 |
| H3 GET 64 KiB | 289 | 5,793 | 0.0493 (0.0461–0.0608) | 5.40 | 0.667 | 8.35 | 330 | 10,334 |

两组 RPS CI 均远低于 1.0，且差异远大于 5%；在这个共同串行点上 Nginx 明确更快。
1 KiB 下 lite 的 SUT CPU 中位利用率仅 9.1%，同时 p99 接近 30 ms，说明主要限制不是
单核算力耗尽，优先怀疑 pacing/timer/事件唤醒路径。64 KiB 下 lite CPU 为 87.4%，更接近
单核饱和，CPU efficiency 仅为 Nginx 的配对中位 `0.0317`。

资源与截止状态：

| Case | 实现 | CPU s/Mreq | SUT peak MiB | client CPU % | backend CPU % | cutoff status 0 |
|---|---|---:|---:|---:|---:|---:|
| GET 1 KiB | lite | 185.1 | 4.8 | 6 | 3.8 | 8 |
| GET 1 KiB | Nginx | 59.3 | 8.8 | 39 | 16.4 | 0 |
| GET 64 KiB | lite | 3,026.3 | 4.6 | 7 | 1.6 | 8 |
| GET 64 KiB | Nginx | 96.8 | 9.4 | 41 | 15.8 | 2 |

`status 0` 只出现在 10 秒 timed cutoff 的未完成 stream；汇总延迟和 good RPS 只计算 2xx，
没有把它们重复记为 HTTP error/timeout。client/backend 均有充足余量。

## 5. 高并发、UDP buffer 与 GSO 诊断

### 5.1 为什么没有有效的 `c8×m64` 主表

正式计划参数在本机不可用：

- 早期 quota 运行有持续 CFS throttling，作废；
- 早期 `AllowedCPUs=0` 因 cpuset 未委派而没有实际绑核，作废；
- 修正为单 CPU 后，1 KiB `c8×m16` 的 9 对运行中 Nginx 有 6 轮
  `RcvbufErrors`，累计 638；
- 64 KiB `c8×m16` 的单轮 Nginx `RcvbufErrors` 为 2,917；降至 `c2×m4` 仍有 21，
  `c4×m4` 有 97；只有 `c1×m1` 可重复保持双方为 0。

这些运行保存在原始数据中用于说明环境瓶颈，不进入性能排名，也不通过选择其中零丢包的
个别轮来生成结果。计划明确禁止静默提高 sysctl，因此本轮没有修改 socket buffer 上限。

### 5.2 GSO on/off

双方服务端和客户端同时切换 GSO，QPACK=0、单 CPU、`c1×m1`、64 KiB GET，每点 3 次：

| 实现 | GSO off RPS | GSO on RPS | on/off | p99 off/on ms |
|---|---:|---:|---:|---:|
| lite-nginx | 285 | 297 | 1.041 | 5.45 / 3.90 |
| Nginx | 4,929 | 6,430 | 1.305 | 1.52 / 0.386 |

3 次诊断样本不足以做正式显著性结论。可以确定的是：lite 开启当前 UDP GSO/sendmmsg
路径没有消除数量级差距；后续应继续分析非 GSO 的 pacing、packet assembly、timer 和
每响应 CPU 成本。

## 6. Churn 与进程健壮性

测试为 lite-nginx `steal auto` 和 `steal off` 各 20 轮。每轮重新启动服务，使用
8 connections × 64 streams 对 1 KiB/64 KiB 做 3 秒 HTTP/3 mixed closed-loop load，
压测器停止形成集中断连，然后检查进程和新连接健康请求。

| 配置 | 轮数 | load/health 失败 | crash/assert/core/hang | 最大 MemoryPeak |
|---|---:|---:|---:|---:|
| `steal auto` | 20 | 0 | 0 | 47.82 MiB |
| `steal off` | 20 | 0 | 0 | 47.04 MiB |

因此 HTTP/3 这组短时集中断连没有复现前序 HTTP/2 报告中的跨 event-loop close 断言。
每轮都是新进程，结果证明短时生命周期门槛通过，但不能替代单进程长时间 memory soak。

## 7. 未执行项与结论边界

按计划停止规则，下列项目没有作为通过项：

- POST 64 KiB 性能矩阵：lite 正确性 gate 已失败；
- fixed-RPS 30/50/70/85/95% 曲线：没有取得三个 endpoint 的共同正确 Rmax；
- 100,000 full handshakes、RESET/STOP_SENDING、完整可认证 H3/QPACK 边界 frame 集；
- 2×/5× Rmax 过载和完整上游故障恢复矩阵；
- 2 小时混合 soak：连续 key update 门槛未满足，且 POST 错误；
- ASan/UBSan/TSan 子集：短时正确性门槛先失败；
- Retry、resumption 和 0-RTT 对比；
- 第二套非 ngtcp2 QUIC 实现的互操作确认。

所以本报告不声称“HTTP/3 全面互操作通过”“异常输入全面可恢复”“2 小时稳定”或
“生产容量”。所有性能数字仅适用于这台 WSL2 主机、本机 loopback、当前内核 UDP buffer、
固定客户端和配置。

## 8. 验证、原始数据和复现

额外验证：

- lite GSO off/on 配置检查均成功；
- Nginx GSO off/on `nginx -t` 均成功；
- benchmark shell `bash -n` 和统计脚本 `py_compile` 成功；
- `ctest --test-dir build --output-on-failure`：1353 passed、2 个显式 Nacos
  interoperability test skipped、0 failed。

原始结果约 2.8 GiB，未提交 Git。主要目录：

```text
temp/http3-benchmark-results/
├── formal-final-qpack0-c1m1-20260719/  # 最终 9 对 GET、CSV、环境与 unit 快照
├── gso-final-qpack0-c1m1-20260719/     # 最终 GSO 诊断
├── protocol-final-affinity-20260719/    # 精确 body 和健壮性子集
├── churn-final-affinity-20260719/       # 20+20 churn
├── preflight-h64-c*/                    # 连接/stream 与 UDP drop 定位
└── formal-valid-h1-c8m16-20260719/      # 有 RcvbufErrors 的无效高并发诊断
```

复现入口：

```bash
# 最终零丢包串行配对矩阵
H3_CLIENTS=1 H3_STREAMS=1 LOAD_THREADS=1 \
IMPLEMENTATIONS='lite nginx' CASE_FILTER=GET REPETITIONS=9 \
DURATION=10 WARMUP=2 COOLDOWN=1 SUT_CPUS=0 SUT_QUOTA=none \
RUN_ID=formal-final-qpack0-c1m1-rerun \
  scripts/benchmark/http3/run_matrix.sh

# 协议/响应与有界扰动
SUT_CPUS=0 SUT_QUOTA=none \
  scripts/benchmark/http3/run_protocol_checks.sh

# 集中断连
ROUNDS=20 DURATION=3 SUT_CPUS=0 SUT_QUOTA=none \
  scripts/benchmark/http3/run_churn.sh

# 重新汇总
scripts/benchmark/http3/summarize.py \
  temp/http3-benchmark-results/formal-final-qpack0-c1m1-20260719
```

## 9. 后续优先级

1. 为 H3 POST echo 增加集成回归，定位 `ProxyHandler` 上游 body writer 为何提前进入
   `Already` 状态；修复后先校验长度和 hash；
2. 分析 1 KiB `c1×m1` 下低 CPU、约 30 ms p99，检查 QUIC pacing deadline、timer
   resolution、ACK/发送唤醒和单 stream 调度；
3. 分析 64 KiB 约 3,026 CPU s/Mreq，使用 profiler 拆分 packet assembly、AEAD、
   send scheduler、系统调用和上游复制成本；
4. 给 benchmark driver 增加可确认的多次 key update、连接/shard 归属与 packet/PTO/cwnd
   统计，并引入一套非 ngtcp2 客户端；
5. 修复正确性后，在明确记录并统一调整 UDP socket buffer 的独立环境中重新发现 Rmax，
   再跑 fixed-RPS 曲线、完整故障矩阵和 2 小时 soak。
