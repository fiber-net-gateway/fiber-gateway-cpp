# lite-nginx HTTP/3 修复后单机复压报告

执行日期：2026-07-20  
代码版本：`c12b43b4941936d1b9c6b281e1b333508710baad`，dirty tree  
对应计划：[单机复压计划 v2](lite_nginx_nginx_single_host_http3_rebenchmark_plan.md)

## 1. 结论

本轮可以确认 `steal on` 修复已命中 remote steal 路径，并通过取消、upstream 异常、backend
重启和进程 churn 回归；HTTP/3 精确响应、1% loss、无效 datagram、默认 poller 与 timerfd
短连接也通过。

本轮不能更新正式性能基线。按计划执行容量发现时：

- `c2 x m4` 的 64 KiB GET/POST 产生 UDP receive-buffer drop；
- `c1 x m1` 的 15 秒确认仍有超过 5% 的 RPS CV；
- proxy POST 64 KiB 在启用 h2load warmup 时可进入“预热有大量流量、测量期 0 请求”的
  静默停顿，h2load 仍返回 0，SUT 仍 active，UDP drop 为 0。

因此 Phase D 的 9 组正式排名、30 分钟/2 小时 soak 均按停止规则未执行。下面的 fixed-RPS
和消融数据只用于趋势与定位，不是正式 lite-nginx/Nginx 排名。

## 2. 环境与可追溯性

- SUT CPU：`0,2`；load generator：`6,8,10,12`；backend：`14,16`；均不设 CFS quota。
- 主机 UDP `rmem_max/wmem_max` 保持 `212992`，未修改 sysctl。
- lite-nginx 使用本轮 Release/LTO 的 GSO off/on、timerfd 和 trace 构建；每个结果目录保存
  Git 状态、CMake cache、命令参数、版本与二进制 SHA-256。
- Nginx 使用仓库固定的 1.31.3 源码与 `temp/nginx-install/sbin/nginx`。
- 所有性能目录均记录 dirty tree，所以只属于开发 smoke/诊断。

## 3. 代码与协议门禁

| 门禁 | 结果 |
|---|---|
| 针对性 CTest | 449/449 pass，Nginx 环境 interop 1 项 skip |
| 全量 CTest | 1445/1445 pass，4 项环境 interop skip |
| `QuicClientTest.UnknownDcidStatelessResetUsesEndpointTokenIndex` | 修正 RFC 允许的 Draining -> Closed 时间竞争后，100/100 pass |
| 仓库 `Http3Client` -> 固定 Nginx | pass |
| GET 1 KiB / GET 64 KiB / POST echo 64 KiB | on/off/Nginx 的 status、长度、SHA-256 全部 pass |
| 第二 HTTP/3 client | on/off/Nginx 全部 pass |
| 1% client TX/RX loss | on/off/Nginx 全部 pass |
| 120 个有界无效 datagram 后健康检查 | on/off/Nginx 全部 pass |
| 100 次默认 poller + 100 次 timerfd 短连接 | 200/200 pass，服务保持 active |

协议原始结果：
[`protocol-c12b43b49419-dirty-221031`](../temp/http3-benchmark-results/protocol-c12b43b49419-dirty-221031/)。
短连接原始结果：
[`short-connections-c12b43b49419-dirty-223258`](../temp/http3-benchmark-results/short-connections-c12b43b49419-dirty-223258/)。

连续 key update 驱动在 on/off/Nginx 上均未完成两次确认；NAT rebinding 仅在一次 steal-off
运行中成功。它们继续作为客户端驱动待办，不作为服务端 pass/fail，也不影响本轮 steal
正确性门禁。

## 4. `steal on` 专项结果

trace 预检得到：

```text
local_hit=0 remote_attempt=1 remote_hit=1 remote_attempt_miss=0 no_candidate=1
```

这证明 workload 实际命中了跨 event-loop 的 remote steal，而不只是普通 local reuse。

| Case | on/off 结果 |
|---|---|
| delayed cancel | 各 8 轮 x 128 请求，约 1024 次取消；全部可恢复 |
| blocked cancel | 各 100 轮 x 8 请求；每轮健康检查成功 |
| upstream close/partial | 进程保持 active，后续健康检查成功 |
| backend restart | 两种配置均恢复，健康检查成功 |
| process churn | on/off 各 50 轮，100/100 无 load/health/UDP 错误 |

专项 runner 共记录 224 条 case 结果，无健康检查失败或进程退出。50 轮 churn 的 SUT
memory peak 范围为：steal-on `4,964,352..5,648,384` bytes，steal-off
`5,296,128..5,914,624` bytes；短测试中未观察到随轮次持续增长。

原始结果：
[`steal-regression-bounded-c12b43b49419-dirty-221858`](../temp/http3-benchmark-results/steal-regression-bounded-c12b43b49419-dirty-221858/)、
[`churn-50-c2m1-c12b43b49419-dirty-222548`](../temp/http3-benchmark-results/churn-50-c2m1-c12b43b49419-dirty-222548/)。

未执行 30 分钟单进程 long-lived churn。正式容量门禁失败后，继续跑长 soak 不符合计划的
阶段顺序。

## 5. 容量发现与停止条件

### 5.1 `c1 x m1`，5 秒 warmup + 15 秒测量

| Case | 实现 | valid | RPS median | RPS CV | 判定 |
|---|---|---:|---:|---:|---|
| GET 1K | steal-on | 3/3 | 388.3 | 5.25% | CV fail |
| GET 1K | steal-off | 3/3 | 402.7 | 3.96% | pass |
| GET 1K | Nginx | 3/3 | 11636.3 | 24.64% | CV fail |
| GET 64K | steal-on | 3/3 | 1383.3 | 1.03% | pass |
| GET 64K | steal-off | 3/3 | 1371.1 | 6.60% | CV fail |
| GET 64K | Nginx | 3/3 | 7469.5 | 6.06% | CV fail |
| POST 64K | steal-on | 2/3 | 1209.5 | 0.54% | missing run |
| POST 64K | steal-off | 2/3 | 1145.9 | 8.91% | missing run + CV fail |
| POST 64K | Nginx | 3/3 | 742.9 | 4.86% | pass |

没有一个 case 同时满足“三个实现均 3/3 valid 且 CV <= 5%”，所以没有正式 `Rcommon`。
原始结果：
[`capacity-confirm-c1m1-c12b43b49419-dirty-230833`](../temp/http3-benchmark-results/capacity-confirm-c1m1-c12b43b49419-dirty-230833/)。

### 5.2 `c2 x m4` 停止点

GET 1 KiB 没有 UDP drop；64 KiB 已超过当前 loopback UDP buffer 能力：

| Case | steal-on | steal-off | Nginx |
|---|---:|---:|---:|
| GET 64K `UdpRcvbufErrors` | 88 | 40 | 429 |
| POST 64K `UdpRcvbufErrors` | 2735 | 2496 | 3573 |

因此没有继续 `c4 x m8` 及更高档。原始结果：
[`capacity-c2m4-c12b43b49419-dirty-223937`](../temp/http3-benchmark-results/capacity-c2m4-c12b43b49419-dirty-223937/)。

## 6. fixed-RPS 诊断曲线

在此前短样本的保守 RPS 上运行了 30%/50%/70%/85% 四档，每点 3 轮、2 秒 warmup、
5 秒测量。汇总器现在要求 fixed-RPS 完成率在 99%--101%，避免 h2load 状态 0 却欠发或
明显超发时误算 valid。总计 104/108 valid，所有点 UDP error 为 0。

最高档的 p99 中位数如下：

| Case / offered RPS | steal-on | steal-off | Nginx |
|---|---:|---:|---:|
| GET 1K / 309 | 28.262 ms | 28.279 ms | 1.192 ms |
| GET 64K / 1149 | 1.006 ms | 1.104 ms | 0.764 ms |
| POST 64K / 771 | 1.261 ms | 1.289 ms | 1.420 ms |

GET 1 KiB 存在明显尾延迟台阶：182 RPS 时 lite 的 p99 约 1.4 ms、p999 约 27 ms；
255--309 RPS 时部分/全部 lite 样本的 p99 升到约 28 ms，Nginx 未出现相同台阶。这是后续
timer/pacing 诊断目标，不能从 5 秒样本推导正式差距。

POST 64 KiB 在 272/454 RPS 有 4 个欠发样本，635/771 RPS 又恢复完整，说明该现象不是
单调的容量饱和。原始结果：
[`fixed-rps-smoke-c12b43b49419-dirty-224539`](../temp/http3-benchmark-results/fixed-rps-smoke-c12b43b49419-dirty-224539/)。

## 7. QUIC 单变量消融

消融固定 GET 64 KiB、675 offered RPS、GSO off 基线，每项 5 组；比值为左/右的配对中位数。

| 变量 | valid pairs | RPS ratio | p99 ratio | CPU efficiency ratio | 结论边界 |
|---|---:|---:|---:|---:|---|
| direct GSO on/off | 4/5 | 1.000 | 0.976 | 1.158 | 吞吐固定；GSO 降低发送 CPU，1 轮驱动欠发 |
| direct timerfd/default | 4/5 | 1.000 | 0.946 | 0.979 | fallback 正确；短样本无明显退化 |
| proxy lite GSO on/off | 5/5 | 1.000 | 0.984 | 1.148 | 稳态 64 KiB CPU 收益 |
| Nginx GSO on/off | 5/5 | 1.000 | 1.021 | 0.986 | 此低负载点无稳定收益 |
| Nginx multi_accept on/off | 5/5 | 1.000 | 0.968 | 0.981 | 区间跨过无差异；保留 off 基线 |
| direct 2 worker/1 worker | 5/5 | 1.000 | 1.013 | 1.034 | 单连接下不能说明多连接扩展性 |

原始结果：
[`ablation-direct-c12b43b49419-dirty-225942`](../temp/http3-benchmark-results/ablation-direct-c12b43b49419-dirty-225942/)、
[`ablation-proxy-c12b43b49419-dirty-230241`](../temp/http3-benchmark-results/ablation-proxy-c12b43b49419-dirty-230241/)、
[`ablation-workers-c12b43b49419-dirty-230618`](../temp/http3-benchmark-results/ablation-workers-c12b43b49419-dirty-230618/)。

## 8. 新发现：proxy POST warmup 边界停顿

容量确认的 POST 64 KiB 第 2 组中，steal-on/off 都出现：

- 5 秒 warmup 已传输约 139/156 MiB request data；
- 后续 15 秒测量为 `0 total, 0 failed, 0 errored, 0 timeout`；
- h2load exit status 为 0、stderr 为空；
- SUT 保持 active，journal 无错误，UDP counter 增量为 0；
- 同组 Nginx 完成 11145 个测量请求。

定位对照：

- 直连 `http3_benchmark_server`、相同 warmup/测量：5/5 valid，RPS CV 0.47%；
- lite proxy 去掉 warmup、连续测量 15 秒：on/off 共 10/10 valid，RPS CV 4.57%/4.03%。

所以纯 QUIC echo 路径没有复现，停顿与 proxy body/upstream 路径及 warmup 切换时刻相关。
现有证据不能区分 h2load warmup 边界行为与 lite proxy 的偶发状态机竞争；在增加 stream、
upstream exchange 与 QUIC packet timeline 前，不应宣称根因已定。

原始结果：
[`post-stall-direct-c12b43b49419-dirty-232001`](../temp/http3-benchmark-results/post-stall-direct-c12b43b49419-dirty-232001/)、
[`post-stall-proxy-nowarm-c12b43b49419-dirty-232203`](../temp/http3-benchmark-results/post-stall-proxy-nowarm-c12b43b49419-dirty-232203/)。

## 9. Nginx 对齐说明

本轮按固定 Nginx 1.31.3 源码拆分了两个单变量：QUIC receive drain 的
`multi_accept off/on`，以及 1-RTT 稳态 GSO off/on。架构对照仍使用
`lite-steal-off / Nginx`，因为 Nginx upstream keepalive cache 是 worker 进程内状态；
`lite-steal-on/off` 另行比较产品默认能力。没有修改仓库管理的 Nginx 源码。

## 10. 后续门禁

再次进入正式复压前应先完成：

1. 为 proxy POST warmup 停顿增加跨 h2load stream、HTTP/3 exchange、upstream exchange 的
   有界 timeline，并能把“驱动没有提交”与“已提交但未完成”区分开；
2. 修复或规避停顿后重跑 `c1 x m1` 3 轮 x 15 秒，要求三个实现全部 valid 且 CV <= 5%；
3. 若仍受 212992-byte UDP buffer 限制，正式选择串行共同点，或经明确授权后在独立环境提高
   buffer 并重跑完整矩阵；
4. 只在上述门禁全绿且 clean tree 构建后执行 9 x 30 秒正式矩阵和 soak。
