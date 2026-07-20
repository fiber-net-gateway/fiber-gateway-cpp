# lite-nginx HTTP/3 修复后单机复压计划（v2）

状态：日常 smoke 与诊断已执行；正式容量门禁未通过  
制定日期：2026-07-20  
关联资料：[原始计划](lite_nginx_nginx_single_host_http3_benchmark_plan.md) / [原始报告](lite_nginx_nginx_single_host_http3_benchmark_report.md) / [问题根因与修复](lite_nginx_http3_benchmark_issue_root_cause_and_fixes.md)

执行结果见：[复压报告](lite_nginx_nginx_single_host_http3_rebenchmark_report.md)。

## 1. 本轮要回答的问题

本轮不再追求一张覆盖所有功能的“大矩阵”，而是把结论拆成三个互不替代的部分：

1. **回归正确性**：今天的 HTTP/3 body/FIN、亚毫秒 timer、QUIC client，以及
   `steal on` 下活动 HTTP/1.1 I/O 取消修复，是否能稳定通过真实反代流量；
2. **单变量收益**：`steal on/off`、UDP GSO on/off、默认 `epoll_pwait2` 与强制
   `timerfd` fallback 各自改变了什么，避免把多个开关的效果混成一个 RPS；
3. **当前性能基线**：在本机共同零 UDP drop、客户端和 backend 都有余量的负载下，
   lite-nginx 与仓库固定 Nginx 的吞吐、尾延迟、CPU 效率和内存差异是多少。

计划只支持同机、同构建、同负载下的开发回归和配对比较，不推导公网或生产容量。

## 2. 为什么旧方法不能直接重跑

- 旧正式点是两个 worker 共用 CPU 0 的 `c1 x m1`。它适合观察 QUIC 串行请求，但不能
  稳定覆盖跨 event-loop 的 connection steal；新版的 steal 主测试必须让两个 worker
  分别运行在两个物理 core 上，并用计数证明发生过 remote steal。
- 旧 `run_matrix.sh` 中名为 `lite` 的实现实际使用 `steal off`，`lite-auto` 才会在两个
  worker 下解析为 on。新版统一使用明确名称 `lite-steal-on` 和 `lite-steal-off`，正式
  结果中不使用 `auto` 作为标签。
- 当前 `build-bench-h3-off/apps/lite_nginx` 生成于 2026-07-20 03:04，早于晚间的
  `f924b8d`/`bab758a` steal 相关修复。任何复压都必须从当前 Git SHA 重新构建，不能沿用
  现有二进制。
- 本机 UDP `rmem_max/wmem_max` 均为 212992 bytes。旧高并发结果出现过
  `RcvbufErrors`；新版先发现每个 case 的共同无丢包负载，再进入正式比较。
- 旧 3 次 x 5 秒数据只用于修复 smoke test。正式结论恢复到至少 9 组配对样本，并把
  fixed-RPS 延迟曲线和 closed-loop 容量分开。

## 3. 两种拓扑和比较边界

### 3.1 QUIC/HTTP/3 直连拓扑

```text
h2load / bsslclient -> HTTP/3 benchmark server
```

该拓扑不经过 upstream pool，只回答 QUIC/HTTP/3 的 timer、pacing、收发 batching、GSO、
握手和 stream 调度问题。正式执行前应增加一个无日志、固定 1 KiB/64 KiB response、64 KiB
echo 的 benchmark server；它复用 `Http3Server`，支持明确的 1/2 worker 配置。现有
`https_echo` 可用于 smoke test，但单 event loop 和通用 echo handler 不作为正式容量结果。

### 3.2 反代拓扑

```text
h2load -> HTTP/3 SUT -> HTTP/1.1 keepalive -> http_benchmark_backend
```

该拓扑回答代理 body 完成语义和 connection pool 的问题。`steal on/off` 只在此拓扑比较。

Nginx 1.31.3 作为固定参考实现。其 QUIC receive 路径逐个调用 `recvmsg`，是否继续 drain
由 `multi_accept` 控制（`temp/nginx-1.31.3/src/event/quic/ngx_event_quic_udp.c:51-65,85`）；
GSO 只允许在已验证 path 且没有 Initial/Handshake 待发数据时使用
（`temp/nginx-1.31.3/src/event/quic/ngx_event_quic_output.c:276-311`）。因此：

- GSO 只比较预热后的 1-RTT 稳态，不把握手混入 GSO 收益；
- Nginx `multi_accept off/on` 先做 3 次诊断，正式配置使用零 drop 且表现更稳定的一项；
- Nginx 的 upstream keepalive cache 是 worker 进程内状态。架构对齐表以
  `lite-steal-off` 对 Nginx；产品默认能力另以 `lite-steal-on` 对 `lite-steal-off`。

## 4. 固定环境

当前主机有 20 个逻辑 CPU、10 个 core、每 core 2 个 SMT sibling。正式测试避开 sibling：

| 角色 | CPU | 限制 |
|---|---|---|
| SUT 两个 worker | `0,2` | `CPUAffinity=0,2`，不设 CFS quota |
| load generator | `6,8,10,12` | `taskset`，最多 4 threads |
| backend | `14,16` | `CPUAffinity=14,16`，不设 quota |
| runner/采样 | `18` | 低频采样，不运行 qlog |

额外保留 `SUT_CPUS=0`、两个 worker 共用一个 CPU 的兼容点，用来和旧报告衔接；它不用于
判断 steal 的扩展性。

不得在主结果中静默修改 sysctl。若以后获准提高 UDP buffer，必须作为独立环境重新跑完整
配对矩阵，不能和当前 212992-byte 基线合表。

## 5. 构建和可追溯性

每次正式运行以当前 commit 命名全新构建目录，例如：

```bash
sha="$(git rev-parse --short=12 HEAD)"

cmake -S . -B "temp/build-bench-${sha}-gso-off" \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_BUILD_APPS=ON \
  -DFIBER_BUILD_EXAMPLES=ON \
  -DFIBER_BUILD_TESTS=OFF \
  -DFIBER_ENABLE_HTTP3=ON \
  -DFIBER_ENABLE_UDP_GSO=OFF \
  -DFIBER_ENABLE_LTO=ON \
  -DFIBER_USE_JEMALLOC=OFF
cmake --build "temp/build-bench-${sha}-gso-off" \
  --target fiber_app_lite_nginx http_benchmark_backend -j

cmake -S . -B "temp/build-bench-${sha}-gso-on" \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_BUILD_APPS=ON \
  -DFIBER_BUILD_EXAMPLES=ON \
  -DFIBER_BUILD_TESTS=OFF \
  -DFIBER_ENABLE_HTTP3=ON \
  -DFIBER_ENABLE_UDP_GSO=ON \
  -DFIBER_ENABLE_LTO=ON \
  -DFIBER_USE_JEMALLOC=OFF
cmake --build "temp/build-bench-${sha}-gso-on" \
  --target fiber_app_lite_nginx -j
```

另建一个 GSO off、`FIBER_FORCE_TIMERFD_POLLER=ON` 的诊断版本，只验证 fallback，不进入
Nginx 排名。Nginx 只使用 `scripts/build_nginx.sh` 固定的 1.31.3 源码和二进制。

每个结果目录必须保存 Git SHA/dirty 状态、CMake cache、编译器、link command、全部二进制
和配置 SHA-256、Nginx `-V`、h2load/bsslclient 版本以及实际命令。正式报告要求 clean tree；
开发 smoke test 可记录 dirty tree，但必须显式标注。

## 6. 执行前需要补齐的压测设施

在跑正式数据前先对 `scripts/benchmark/http3/` 做以下小改造：

1. 增加显式 `lite_nginx_steal_on.conf`，不再用 `auto` 代替 on；
2. 把实现名称改为正交维度：`lite-steal-on/off`、`gso-on/off`、`poller-default/timerfd`，
   禁止一个名称同时隐含多个开关；
3. 增加 fixed-RPS 模式，使用现有 h2load 的 `--rps`，并保存 offered/completed RPS；
4. 增加 H3 upstream fault runner，复用 backend 的 `/fault/delay`、`/fault/hang`、
   `/fault/close` 和 `/fault/partial`；
5. 增加可关闭的诊断构建计数：local/remote pool hit、steal attempt/hit/miss、remote return、
   new upstream connection、active I/O cancel；QUIC 侧记录 recv/send batch、GSO segments、
   pacing wait 次数和 deadline overshoot；
6. trace 版本只用于证明测试确实命中目标路径。正式性能数据使用无 trace 的 Release/LTO
   二进制，避免计数和日志污染热路径；
7. runner 启动时校验二进制来自本轮构建目录，并拒绝端口占用、旧 PID、缺失 cgroup 统计、
   配置检查失败或无法读取 UDP counters 的运行。

## 7. 执行阶段

### Phase A：代码和协议门禁

先运行针对性测试，再运行全量 CTest：

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure \
  -R 'StealableHttp1ConnectionPoolSetTest|ClientHttp1ExchangeTest|Http3|Quic|PollerTest'
ctest --test-dir build --output-on-failure
```

随后用 h2load、bsslclient、仓库 `Http3Client` 分别完成：

- GET 1 KiB、GET 64 KiB、POST echo 64 KiB 的 status、长度和 SHA-256；
- FIN 与最后 DATA 同批、FIN 延后、截断 DATA、client reset；
- lite client -> Nginx、独立 h2load/bsslclient -> lite 的双向互操作；
- 默认 poller 与强制 timerfd 各 100 次短连接；
- 1% client TX/RX loss 和有界无效 datagram 后健康检查。

连续 key update 和 NAT rebinding 仍受客户端驱动时序影响。在驱动能明确等待 key phase
确认、记录 PATH_CHALLENGE/PATH_RESPONSE 前，它们作为独立待办，不阻断本轮 steal 和稳态
性能复压，也不得写成服务端通过。

### Phase B：`steal on` 专项回归

使用两个 worker、两个 SUT core。trace 预检必须满足 `remote_steal_hit > 0`，否则该轮只是
普通代理流量，不能证明 steal 修复有效。

| Case | 方法 | on/off 各自要求 |
|---|---|---|
| balanced reuse | `c8 x m16` 1 KiB GET，30 秒 | 0 error；记录 local/remote hit 和新建连接 |
| delayed cancel | `/fault/delay`，随机在 50-200 ms 断开客户端 | 1000 次取消，无 assert/UAF/hang |
| blocked cancel | `/fault/hang` 后集中结束客户端 | 100 轮，服务在 1 秒内可接受健康请求 |
| upstream abort | `/fault/close`、`/fault/partial` | 返回预期 5xx/stream error，进程继续 active |
| backend restart | 满载中停止并重启 backend | 5 秒内恢复，旧连接不回池复用 |
| process churn | 启动、3 秒混合负载、停客户端、健康检查、停 SUT | on/off 各 50 轮全部成功 |
| long-lived churn | 单个 SUT 进程内重复取消和恢复 | 30 分钟，无持续 RSS/idle pool 增长 |

专项回归只比较正确性、恢复时间、连接复用率和资源稳定性。`steal on` 的 5xx 不能因为
`steal off` 正常而被排除；任何 crash、assert、ASan/UBSan 报告或健康检查失败都立即停止
后续性能测试。

### Phase C：共同无丢包容量发现

分别对 1 KiB GET、64 KiB GET、64 KiB POST，从以下点逐步提高 closed-loop 并发：

```text
c1xm1 -> c2xm4 -> c4xm8 -> c8xm16 -> c8xm32
```

每点预热 5 秒、测量 15 秒、重复 3 次。依次测试 `lite-steal-on`、
`lite-steal-off`、Nginx，使用轮转顺序而不是连续跑完某个实现。

一个点只有同时满足以下条件才是 valid：

- gate/load status 为 0，所有已完成请求均为预期 2xx 和 body；
- timeout、failed、errored、5xx 为 0；
- `UdpInErrors`、`UdpRcvbufErrors`、`UdpSndbufErrors` 增量为 0；
- SUT 无 cgroup throttling，backend CPU 低于两个 core 的 50%；
- 客户端 CPU 低于分配预算的 70%，连接没有 inactivity timeout；
- 三次 RPS 变异系数不超过 5%。

每个 case 取三个实现都 valid 的最高点作为“共同容量点”。若只有 `c1 x m1` 有效，则明确
称为串行性能点，不称 Rmax；不得从带 UDP drop 的点挑选个别好样本。

### Phase D：正式配对矩阵

在各 case 的共同容量点运行 9 组，每轮预热 15 秒、测量 30 秒、drain 5 秒、冷却 10 秒。
三个实现按固定 Latin-square 顺序轮转：

```text
on -> off -> nginx
off -> nginx -> on
nginx -> on -> off
```

| Case | 首要定位 | 主指标 |
|---|---|---|
| H3-P-GET-1K | 小响应、pacing/timer、upstream reuse | good RPS、p99/p999、req/CPU-s |
| H3-P-GET-64K | packet assembly、GSO 前基线、发送 CPU | RPS、MiB/s、p99、CPU/MiB |
| H3-P-POST-64K | body/FIN 修复、双向流控、代理完成语义 | good RPS、hash、p99、req/CPU-s |

比较分两张表：

- `lite-steal-on / lite-steal-off`：默认能力的收益和成本；
- `lite-steal-off / nginx`：更接近两端 per-worker upstream keepalive 的架构对照。

报告 median、min/max、配对比值和固定 seed bootstrap 95% CI。9 组后若配对 RPS 变异系数
仍大于 5%，按预先规则扩到 13 组；不允许删除无错误但“难看”的有效轮次。

### Phase E：fixed-RPS 延迟曲线

以 Phase C 三个实现中最小 valid closed-loop RPS 为 `Rcommon`，对每个 case 运行
`30%/50%/70%/85% Rcommon`。每点 3 组、预热 10 秒、测量 30 秒。

`95%` 只作过载诊断，不进入正常延迟图。主图使用相同 offered RPS 对比 p50/p95/p99/p999、
完成率和 CPU，避免 closed-loop 在慢实现上自动降低请求压力而产生 coordinated omission。

### Phase F：QUIC 单变量消融

只在直连拓扑和反代的 64 KiB 稳态各运行 5 组：

| 变量 | 固定条件 | 结论边界 |
|---|---|---|
| GSO off/on | 已建立连接、相同 offered RPS、客户端与服务端同步切换 | 只说明稳态 GSO 收益 |
| default poller/timerfd | GSO off、相同 pacing | 验证 fallback 不退化或失真 |
| 1 worker/2 workers | 每 worker 一个物理 core | 观察 shard 扩展，不与 steal 混合 |
| `c1xm1/c1xm16/c2xm16/c8xm16` | 总 stream 数另做等量对照 | 区分 stream 与 connection 扩展 |

不把 `pacing off` 纳入正式开关矩阵。此前它改善 1 KiB 却破坏 64 KiB 完成；只有在新的
connection trace 能证明 burst/cwnd/loss 安全后，才允许作为独立实验重新进入。

### Phase G：soak

前述阶段全绿后执行 30 分钟门禁，再决定是否跑 2 小时：

- 70% `Rcommon`，GET 1 KiB / GET 64 KiB / POST 64 KiB 按固定比例混合；
- 每 5 分钟执行一轮连接重建和一次健康检查；
- 每分钟采集 RSS、cgroup memory、active QUIC connections、pool idle、UDP errors；
- 任何 hash 错误、5xx、timeout、UDP drop、assert 或无法恢复都立即失败；
- 内存应在预热后形成平台，不允许 active connection 已回落而 RSS/pool 持续单调增长。

如果 key-update driver 仍未修好，2 小时长连接不能声称覆盖多次 key update；应限制每连接
packet 数并轮换连接，同时把重连成本单独报告。

## 8. 日常版与正式版

为避免每次修改都跑数小时，保留两个入口：

### 日常 smoke（约 30-45 分钟）

- Phase A 针对性测试和三个精确响应；
- `steal on/off` 各 10 轮 delayed/blocked cancel；
- 三个实现、三个 case、`c1 x m1` 各 3 次 x 5 秒；
- 默认 poller 与 timerfd 各 20 次短连接。

只给出 pass/fail 和趋势，不更新正式性能基线。

### 正式复压（约 4-6 小时，不含 2 小时 soak）

- 全量 Phase A-G；
- 所有正式运行使用 clean tree 和 SHA 命名构建；
- 生成 `runs.csv`、`summary.csv`、`paired-summary.csv`、延迟曲线数据和单独的 invalid-runs；
- 报告同时写明绝对值、配对比值、有效性门槛和未覆盖项。

## 9. 最终验收

本轮完成的最低条件是：

1. `steal on` 专项 workload 被诊断计数证明发生 remote steal，on/off 各 50 轮无崩溃，
   active I/O cancel 后可恢复；
2. GET 1 KiB、GET 64 KiB、POST 64 KiB 全部精确正确，无 5xx/timeout/UDP drop；
3. 至少得到一个三实现共同有效的并发点和一条 30%-85% fixed-RPS 延迟曲线；
4. GSO、poller 和 worker 数只按各自单变量结果下结论；
5. 正式报告不把未修好的连续 key update、NAT rebinding 或 WSL2 loopback 结果外推为生产能力。
