# lite-nginx 与 Nginx 单机 HTTP/1、HTTP/2 压测报告

测试日期：2026-07-19  
代码版本：`22d8e6636688d0f7dec89bdcd788170daafdb9a6`（测试设施为未提交工作树改动）

## 1. 结论摘要

本轮在同一 WSL2 主机、相同 200% CPU quota、相同上游和 TLS 配置下完成了
8 个场景、每方 7 次的平衡配对矩阵，共 112 个正式运行和 290,506,534 个请求。

主要结论：

- lite-nginx 默认的上游连接池 `steal auto` 存在确定的健壮性缺陷：20 次短时
  HTTP/2 高并发后集中断连测试中发生 3 次崩溃，均为
  `src/net/detail/RWFd.cpp:59` 的 `loop().in_loop()` 断言失败，systemd 记录为
  `SIGABRT/core-dump`。因此默认配置未通过稳定性验收。
- 为避免上述缺陷污染性能数据，正式排名使用 `steal off`。该结果是稳定回退配置的
  性能基线，不能直接当作当前默认 `steal auto` 的生产基线。
- HTTP/1 明文 1 KiB 和 HTTP/1 TLS 1 KiB 下 Nginx 更快；64 KiB GET、明文
  POST 64 KiB 下 lite-nginx 明显更快。
- HTTP/2 64 KiB GET 和 POST 的吞吐差异小于 5%，在当前 WSL2 环境下视为不可区分。
- HTTP/2 1 KiB 下 lite-nginx 吞吐更高且波动小，但 Nginx 结果呈明显双峰；8 条
  长连接在两个 Nginx worker 间分配不均，使该场景不能解释为纯协议计算能力差异。
- 正常矩阵中 lite-nginx 为零请求失败、零进程退出。Nginx 有 284 个测量截止时未完成
  的 HTTP/2 stream，占全部正式请求约 0.000098%；没有 HTTP 4xx/5xx、超时或进程退出。
- 异常 HTTP/1 输入、慢头、128 个不读响应的客户端、上游延迟/关闭/部分响应/挂起及
  后端重启测试中，`steal off` 进程保持存活。后端重启后第 1 次探测返回 502，第 2 次
  探测恢复，耗时 0.141 秒。

本轮不能给出“默认配置已通过”的结论。首要工作应是修复跨 event-loop 关闭
`RWFd` 的断言崩溃，然后用同一脚本重新跑 `steal auto` 基线。

## 2. 环境和方法

### 2.1 环境

- WSL2 Linux `6.6.114.1-microsoft-standard-WSL2`；
- Intel Core i7-13700H，WSL2 可见 20 个逻辑 CPU、10 个 core；
- cgroup v2 和 user systemd transient unit；
- lite-nginx：Release、LTO、默认 glibc allocator，二进制 SHA-256
  `3ca89ac18f23116cec2847f803234e6b1b31be739194c178a06f6ace102dad82`；
- Nginx：仓库固定的 1.31.3、Clang 22、BoringSSL，二进制 SHA-256
  `df34817db9620748ca5f505e6ca4b96f87bad87d568b6fccd869221ad7159dfb`；
- 压测器：h2load 1.59.0；
- TLS：双方使用同一证书和 TLS 1.3；
- 双方均为 2 worker、相同 listener、相同 HTTP/1.1 上游、关闭 access log 和
  proxy buffering。

CPU 隔离和预算：

| 角色 | vCPU | quota |
|---|---|---:|
| SUT | `0,2,4` | 200% |
| h2load | `6,8,10,12` | 无额外 quota，4 threads |
| 公共上游 | `14,16` | 200% |
| 监控 | `18` | 无额外 quota |

h2load 在 4 个分配 CPU 上的最高平均利用率为 37.25%，中位数为 27.38%，不是主矩阵
瓶颈。SUT 的 accept/master 成本和 worker 成本均包含在同一个 cgroup quota 中。

### 2.2 正式矩阵

- 每个实现每个场景运行 7 次；
- 每次预热 15 秒、测量 60 秒、冷却 30 秒；
- 奇数轮 `lite -> nginx`，偶数轮 `nginx -> lite`；
- HTTP/1 使用 128 connections、无 pipeline；
- HTTP/2 使用 8 connections、每连接 64 concurrent streams；
- 每次启动后验证 HTTP version、状态码、响应长度和 SHA-256；
- 记录逐请求 TSV、h2load 输出、cgroup `cpu.stat`、memory peak、systemd 状态和日志；
- 表中的 RPS ratio、p99 ratio 和 CPU efficiency ratio 均为 7 个配对比值的中位数；
- RPS ratio 置信区间使用固定随机种子的配对 bootstrap 95% CI。

## 3. 正式性能结果

### 3.1 吞吐、尾延迟和 CPU 效率

| Case | lite RPS | nginx RPS | lite/nginx RPS ratio (95% CI) | lite p99 ms | nginx p99 ms | p99 ratio | lite req/CPU-s | nginx req/CPU-s | failed lite/nginx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| H1-P-1K | 68,941 | 94,743 | 0.739 (0.725–0.744) | 2.44 | 2.03 | 1.165 | 37,593 | 51,214 | 0 / 0 |
| H1-P-64K | 40,614 | 22,617 | 1.810 (1.791–1.825) | 4.03 | 7.42 | 0.565 | 22,337 | 12,326 | 0 / 0 |
| H1-P-POST | 24,226 | 16,271 | 1.489 (1.468–1.519) | 7.39 | 10.02 | 0.741 | 13,232 | 8,873 | 0 / 0 |
| H1-T-1K | 64,249 | 70,863 | 0.908 (0.900–0.915) | 2.51 | 2.35 | 1.069 | 35,182 | 38,586 | 0 / 0 |
| H1-T-64K | 30,624 | 15,760 | 1.943 (1.908–1.980) | 5.24 | 10.25 | 0.507 | 16,736 | 8,601 | 0 / 0 |
| H2-T-1K | 125,518 | 62,952 | 1.974 (1.483–3.323) | 6.34 | 13.61 | 0.467 | 68,726 | 52,834 | 0 / 284* |
| H2-T-64K | 17,060 | 16,961 | 1.040 (0.994–1.080) | 43.12 | 41.02 | 1.070 | 9,316 | 9,243 | 0 / 0 |
| H2-T-POST | 12,760 | 12,705 | 0.975 (0.968–1.011) | 58.17 | 72.40 | 0.819 | 6,858 | 6,791 | 0 / 0 |

`*` 284 个请求全部是 h2load 在 60 秒截止时记录的未完成 stream（状态 `-1` 或
`0`，并归类为 `errored`），没有 HTTP 4xx/5xx 或 timeout。h2load 的 `errored` 和
`timeout` 是 `failed` 的子集，报告没有重复相加。

差异判断：

- H1-P-1K：7/7 配对均为 Nginx 更快，lite 吞吐低约 26%；
- H1-P-64K、H1-P-POST、H1-T-64K：7/7 配对均为 lite 更快，分别约高 81%、
  49% 和 94%；
- H1-T-1K：7/7 配对均为 Nginx 更快，lite 低约 9%；
- H2-T-1K：7/7 配对均为 lite 更快，但 CI 很宽，必须结合连接分配诊断解释；
- H2-T-64K、H2-T-POST：差异小于 5%，当前环境下不可区分。

### 3.2 每百万请求 CPU 时间和内存

| Case | lite CPU s/Mreq | nginx CPU s/Mreq | lite peak MiB | nginx peak MiB |
|---|---:|---:|---:|---:|
| H1-P-1K | 26.6 | 19.5 | 10.5 | 11.7 |
| H1-P-64K | 44.8 | 81.1 | 16.5 | 19.1 |
| H1-P-POST | 75.6 | 112.7 | 22.0 | 21.5 |
| H1-T-1K | 28.4 | 25.9 | 11.7 | 12.4 |
| H1-T-64K | 59.7 | 116.3 | 17.4 | 20.1 |
| H2-T-1K | 14.6 | 18.9 | 21.7 | 21.6 |
| H2-T-64K | 107.3 | 108.2 | 48.4 | 46.1 |
| H2-T-POST | 145.8 | 147.3 | 178.6 | 85.5 |

H2 POST 的吞吐和 CPU 效率相近，但 lite-nginx memory peak 是 Nginx 的约 2.1 倍，
是后续内存复用和背压分析的优先场景。

### 3.3 HTTP/2 连接数诊断

15 秒测量、3 次运行的中位数：

| 模式 | lite RPS | nginx RPS | 解释 |
|---|---:|---:|---|
| `c1 m64 t1` | 79,308 | 44,589 | 单连接只利用一个 worker，双方均低于多连接结果 |
| `c2 m64 t2` | 142,257 | 58,872 | lite 的两条连接稳定分到两个 worker；Nginx 未呈线性扩展 |
| 主矩阵 `c8 m64 t4` | 125,518 | 62,952 | Nginx 7 次为 34,556–88,304 RPS，明显双峰 |
| `c32 m64 t4` | 109,953 | 77,767 | 两方均出现大量失败，属于过载压力，不是有效吞吐点 |
| `c8 m128 t4` | 无有效值 | 24,113 | lite 有流量但 h2load 记 0 measured requests；该组合结论无效 |

Nginx 主矩阵每轮 SUT CPU 时间随 RPS 从约 55 秒到 90 秒变化，说明部分运行没有同时
压满两个 worker。少量长连接的 worker 落点主导了 H2 1 KiB 结果。lite-nginx 的
accept loop 轮询分配使结果更稳定。这个差异是真实的当前配置行为，但不能据此声称
lite 的 HTTP/2 核心处理逻辑本身快 97%。

## 4. 上游与压测器余量

相同 200% quota 下直压 benchmark backend：

| 端点 | 直压 RPS | 错误 |
|---|---:|---:|
| GET 1 KiB | 222,858 | 0 |
| GET 64 KiB | 117,806 | 0 |
| POST echo 64 KiB | 74,081 | 0 |

上游没有达到 CPU quota，也没有 cgroup throttling 或请求错误；不过严格按原计划的
保守门槛，本轮有两组略微越界：H1-P-1K/Nginx 上游最高使用分配 CPU 的 53.5%，
H2-T-1K/lite 中位数为 51.1%，且 1 KiB 直压容量约为 lite H2 峰值的 1.78 倍，低于
计划要求的 2 倍。因此本报告保留数据作为开发基线，但不把绝对排名标记为完全通过
Phase 4 gate。若需要发布级结论，应把 SUT quota 降到 100% 后重跑。

## 5. 健壮性和故障恢复

### 5.1 `steal auto` 崩溃

测试条件：H2 TLS、8 connections、每连接 64 streams、3 秒持续负载、无预热；每轮
重启服务，共 20 轮。

- 总请求 7,200,067，h2load 请求错误为 0；
- 3/20 轮在压测结束、客户端集中断连时崩溃；
- 三次均为 `FIBER_ASSERT failed: loop().in_loop()`；
- 断言位置均为 `src/net/detail/RWFd.cpp:59 (close)`；
- systemd 均记录 `status=6/ABRT`、`Result=core-dump`；
- `steal off` 的 56 个正式 lite 实例没有复现该断言。

这表明连接窃取后的上游 fd 在错误的 event loop 上执行关闭。修复时应优先审计
`proxy_over_connection` 结束路径和 `RWFdCrossThreadWaiter` 恢复后 fd 的所有权，而不是
只屏蔽断言。

### 5.2 HTTP/1 边界输入和慢客户端

在 512 MiB MemoryMax 下依次测试：

- 冲突的多个 Content-Length；
- Content-Length 与 Transfer-Encoding 同时存在；
- 非法 chunk size、缺失 chunk terminator；
- 声明 64 KiB 但只发送部分 body 后 half-close；
- 32 KiB URI、64 KiB 单 header、2000 个 headers；
- 逐字节慢速发送 request headers；
- 128 个请求 64 KiB 响应但暂不读取的客户端。

每个 case 后的正常请求均返回 200，进程始终 active，最终 memory peak 为 17.1 MiB。
需要注意：lite-nginx 对 `Content-Length + Transfer-Encoding` 没有在下游拒绝，而是
处理后返回 204；超大 header 和大量 header 被转发到上游后返回 502。它们是需要与
Nginx 语义对齐的协议兼容性缺口，但本轮未导致进程级故障。

### 5.3 上游故障

| 故障 | 观察结果 | 故障后健康检查 |
|---|---|---|
| 延迟 250 ms | 1,216/1,216 成功，约 243 RPS | 通过 |
| 接收后关闭 | 下游返回 5xx，进程存活 | 通过 |
| 发送部分 body 后关闭 | h2load 记录 stream error，进程存活 | 通过 |
| 挂起不响应 | 测量期请求保持 pending，正常请求仍可处理 | 通过 |
| 后端停止 | 返回 502 | 后端重启后第 2 次探测恢复，0.141 秒 |

稳定回退配置通过了这些短时故障恢复测试，但它们不能替代长时间 soak。

## 6. 完成范围和限制

已完成：Release/LTO 构建、固定 Nginx 源码核对、协议和响应预检、后端余量检查、
完整 7 对吞吐矩阵、配对 bootstrap、HTTP/2 连接数诊断、HTTP/1 边界输入、慢客户端、
上游故障，以及默认连接窃取模式的竞态复现。

以下项目没有作为本轮通过项：

- 原计划的 2 小时 `steal auto` soak：默认模式在 3 秒压力中已 3/20 崩溃，继续长跑
  不会改变验收失败结论；
- 5 分钟 × 多档固定 RPS 曲线：主矩阵已发现默认模式阻断缺陷，留待修复后跑；
- h2spec、自定义非法 HTTP/2 frame/HPACK、ASan/UBSan/TSan：当前机器没有 h2spec，
  且按要求没有下载外部工具；sanitizer 不参与本次性能排名；
- 生产容量、真实网络、网卡和跨机扩展结论：单机 loopback + WSL2 数据不支持这些推断。

## 7. 原始数据和复现入口

原始结果未提交 Git，总量约 7.1 GiB，位于：

```text
temp/http-benchmark-results/formal-20260719/
├── main/                 # 112 个正式运行、CSV、配对统计和环境快照
├── backend-capacity/     # 上游直压
├── h2-scaling/           # HTTP/2 连接/流诊断
├── lite-auto-churn/      # 3/20 崩溃复现
└── robustness-final/     # HTTP/1 边界和上游故障
```

可复现入口：

```bash
# 完整矩阵
scripts/benchmark/http/run_matrix.sh

# 短时协议边界和故障恢复
RESULT_DIR=temp/http-benchmark-results/robustness \
  scripts/benchmark/http/run_robustness.sh

# 重做汇总
scripts/benchmark/http/summarize.py \
  temp/http-benchmark-results/formal-20260719/main
```

## 8. 后续优先级

1. 修复 `steal auto` 下跨 event-loop `RWFd::close()`，加入集中断连回归测试；
2. 明确拒绝冲突请求 framing，并在下游限制 URI/header 大小，避免依赖上游 502；
3. 分析 H2 POST 约 178.6 MiB memory peak，重点检查请求体和流级 buffer 复用；
4. 修复后先跑 20 轮短时 churn，再跑 2 小时 soak 和固定 RPS 延迟曲线；
5. 若要发布性能结论，将 SUT quota 降到 100%，满足上游低于 50% 的严格门槛后重跑。
