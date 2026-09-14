# lite-nginx 反向代理全协议压测复测报告（#5）

执行日期：2026-09-14
被测对象：`apps/lite_nginx` @ git `a0263f3`（Release + LTO + libc++，clang-20，构建配置逐项核对与 #3 一致）
对比对象：OpenResty 1.25.3.2、nginx 1.31.1 (quic)（与 #2/#3 完全相同的构建与配置）
基线：[all_benchmark_3.md](all_benchmark_3.md)（2026-09-08，git `9ecdad5`，同机）
参考：[all_benchmark_4.md](all_benchmark_4.md)（2026-09-12，git `e5ad72c`，**WSL2 异机**，结论仅作方向参考）
方法论：与 #3 完全一致（场景/参数/绑核/公平性配置），多样本取中位数：H1×3、H2×2、H3×6。

> **本次回到 #2/#3 的原压测主机**（16 vCPU / Ubuntu 20.04 / clang-20 / 同一 `temp/bench/` 工具链：wrk 4.2.0、h2load nghttp2/1.65.0、OpenResty 1.25.3.2、nginx 1.31.1），绝对值与 #3 直接可比。距 #3 共 57 个提交，其中与吞吐直接相关的：
> - `65ca6f0` perf(core): schedule continuations with nginx posted_next semantics
> - `65be939` feat(core): persistent epoll ET with user-space readiness tracking
> - `a58cf18` perf(http): fill TLS records across nodes and batch DATA frames per op
> - `bcb57b4` fix(http): start the H1 header timeout at the first byte and drop peer-closed idle upstreams
> - 其余为 QUIC client/endpoint 生命周期重构、IntrusiveList 重设计（c4404b5）、H2 capacity gate 修复等

---

## 1. 结论速览（vs 基线 #3；#4 为异机不直接比）

| 维度 | 本次结论 | vs #3 |
|---|---|---|
| **H2 全部四场景** | lite-nginx **全面反超**：1K 178.6k（+38%）、64K 22.1k（+130%）、1M 1,771（+149%）、POST 852（+138%），离散 ±0~6% | **"H2 大体 harness-bound" 作废**；亦反证 #4 异机发现的 40-45% 缺口已被 `a58cf18` 翻转为 2.0-3.0× 优势 |
| **H3 大体（1M/POST/64K）** | 按档位归一（每 worker）：1M **2.5×**、POST **2.7×**、64K **2.2×**；快档绝对值 1M ~1510 / POST ~750 | #3 "快档持平"；本次 lite 独涨 +76~189%，nginx 侧与 #3 完全一致 |
| **H3 GET 1 KiB** | lite 档位 72-81k/106-109k（2/3 worker），nginx 47-49k/74-77k；**每 worker ~36k vs ~25k（+44%）** | #3 报 88k 单档 vs 83k 快档（+6%）；本次拉开 |
| **POST echo 1 MiB（H1）** | lite 5,380 rps / 5.26 GB/s，为 OpenResty/nginx 的 **2.55×**（对照组为 #2/#3 原配置：请求体缓冲 on） | 保持（#3 2.60×）；#4 已证明若对照开 `proxy_request_buffering off` 则收窄至 ~1.2-1.9× |
| **HTTP/1 GET 1 KiB** | lite 130.4k，落后 OpenResty/nginx（~210k）**38%** | #3 报 44%；lite +11% 缩小差距，落后事实不变 |
| **GET 1 MiB（H1）** | 三者 9.8-10.4k（~10 GB/s）打平 | 不变，带宽封顶 |
| **稳定性** | 全部 108 次有效测量 0 失败 / 0 非 2xx / H3 全程 0 UDP 丢包；wrk 时长全部 20.0x s（无时钟回拨） | 不变 |

一句话：**`a58cf18`（跨节点填满 TLS record + 每操作批量化 DATA 帧）让 lite-nginx 的 H2/H3 大体吞吐翻倍以上——H2 四场景全面反超对照（2.0-3.0×），H3 每 worker 2.2-2.7×；H1 面持平，POST echo 2.55×（对照为缓冲配置）与 GET 1K 落后 ~38% 的旧格局不变。**

---

## 2. 环境与构建（与 #3 的差异点）

- 同一台 16 vCPU / 32 GiB / Ubuntu 20.04 主机（`results2/s1` 原始数据时间戳 2026-09-08 在册）；sysctl 未变（`rmem_max=33MB`、`wmem_max=208KB`）。
- 绑核不变：backend 0-3 / proxy 4-9 / loadgen 10-15。
- 后端、wrk、h2load、OpenResty、nginx-quic、证书、POST 体、代理配置全部复用 #3 的 `temp/bench/`（版本逐一核对：wrk 4.2.0 / nghttp2 1.65.0 / OpenResty 1.25.3.2 / nginx 1.31.1）。
- lite-nginx 从 `9ecdad5` 换到 `a0263f3`；CMakeCache 逐项核对：Release / LTO ON / libc++ / clang++-20 / 无 jemalloc，与 #3 一致。
- 后台负载与 #3 同级（含 7 月起常驻的 idle cat-demo nginx，0% CPU）。
- 本次脚本：`run_matrix5.sh` / `run_h3_5.sh`（写 `results5/s*/`）、`parse_results5.py`（内嵌 #3 中位基线）；其余复用 #3。

---

## 3. 结果（每协议中位数；Δ 相对 #3 数值）

### 3.1 HTTP/1（wrk -t6 -c256 -d20s，3 样本中位）

| 场景 | 代理 | RPS | Δ vs #3 | 离散 | p50 ms | p90 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | lite-nginx | 130,367 | +11% | ±2% | 1.71 | 2.63 | 6.19 |
| GET 1 KiB | OpenResty | **214,076** | +2% | ±1% | 1.11 | 1.55 | 1.89 |
| GET 1 KiB | nginx(quic) | 209,025 | -1% | ±3% | 1.10 | 1.65 | 1.94 |
| GET 64 KiB | lite-nginx | 69,183 | +2% | ±1% | 3.26 | 4.85 | 5.25 |
| GET 64 KiB | OpenResty | 83,354 | -8% | ±3% | 2.76 | 4.07 | 5.22 |
| GET 64 KiB | nginx(quic) | 84,005 | -6% | ±7% | 2.77 | 4.05 | 5.09 |
| GET 1 MiB | lite-nginx | 9,758 | -4% | ±3% | 23.99 | 32.35 | 37.99 |
| GET 1 MiB | OpenResty | **10,151** | -5% | ±2% | 23.09 | 37.85 | 52.03 |
| GET 1 MiB | nginx(quic) | 10,362 | -2% | ±5% | 23.16 | 30.24 | 38.75 |
| POST echo 1 MiB | **lite-nginx** | **5,380** | -3% | ±1% | 40.73 | 69.61 | 94.52 |
| POST echo 1 MiB | OpenResty | 2,107 | -2% | ±2% | 116.10 | 135.30 | 421.27 |
| POST echo 1 MiB | nginx(quic) | 2,142 | +1% | ±3% | 113.92 | 144.18 | 304.35 |

样本明细（3 样本）：lite get1k 127.7/130.4/134.2k；OR 211.4-217.4k；nginx 203.0/209.0/215.1k。全部 0 错误 / 0 非 2xx，时长 20.0x s。

**要点**

- **H1 面整体持平**：三代理各格变化 -8%~+11%，均在 #3 记录的运行间方差内；期间穿插的 epoll 持久化/调度/IntrusiveList 重构对 H1 无净影响。
- **GET 1 KiB**：lite 130.4k（+11%），对照维持 ~210k，落后幅度从 44% 收窄到 38%。p99 6.19ms（#3 7.82ms、#4 4.30ms）仍显著劣于对照 ~2ms——**遗留排查项**（偶发尾延迟尖刺，跨报告持续存在）。
- **POST echo 1 MiB**：5,380 rps / 5.26 GB/s，2.55×（#3 2.60×；lite -3% 与对照 ±2% 在噪声内，判定持平）。注意 #4 的附加实验结论：对照改 `proxy_request_buffering off` 后此倍数收窄至 ~1.2-1.9×——本表倍数是在 #2/#3 原对照配置（请求体缓冲落盘）下的数字。

### 3.2 HTTP/2（h2load -t6 -c32 -m16 -D20s TLS，2 样本中位）

| 场景 | 代理 | RPS | Δ vs #3 | 离散 | BW MB/s | mean ms | max ms |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **178,596** | +38% | ±6% | 187 | 2.84 | 54.17 |
| GET 1 KiB | OpenResty | 121,177 | -3% | ±0% | 129 | 4.28 | 26.10 |
| GET 1 KiB | nginx(quic) | 109,046 | +1% | ±1% | 116 | 4.94 | 32.05 |
| GET 64 KiB | **lite-nginx** | **22,149** | +130% | ±0% | 1,350 | 22.86 | 74.53 |
| GET 64 KiB | OpenResty | 8,582 | -1% | ±0% | 538 | 55.22 | 133.07 |
| GET 64 KiB | nginx(quic) | 11,269 | -2% | ±1% | 706 | 45.34 | 99.97 |
| GET 1 MiB | **lite-nginx** | **1,771** | +149% | ±1% | 1,735 | 286.83 | 498.57 |
| GET 1 MiB | OpenResty | 580 | -1% | ±1% | 581 | 819.69 | 4,845.00 |
| GET 1 MiB | nginx(quic) | 805 | -1% | ±0% | 806 | 624.56 | 1,175.00 |
| POST echo 1 MiB | **lite-nginx** | **852** | +138% | ±2% | 854 | 591.36 | 787.75 |
| POST echo 1 MiB | OpenResty | 272 | -1% | ±0% | 273 | 1,720.00 | 4,165.00 |
| POST echo 1 MiB | nginx(quic) | 374 | -1% | ±1% | 374 | 1,270.00 | 3,210.00 |

全部 0 failed / 0 timeout；两对照各格对 #3 变化 ≤3%（环境等价性成立）。

**要点**

- **本次最大的变化**：lite 四场景全幅 +38%~+149%，全部反超两对照：
  - GET 1K：178.6k，OpenResty 的 1.47×、nginx 的 1.64×。
  - GET 64K：22.1k，OpenResty 的 2.58×、nginx 的 1.97×。
  - GET 1M：1,771（1.70 GB/s），nginx 的 2.20×、OpenResty 的 3.05×；POST：852，nginx 的 2.28×、OpenResty 的 3.13×，mean 延迟亦为三者最低（591ms，#3 为 1,380ms）。
- **与 #4 的衔接**：#4 在 h2load 不构成瓶颈的主机上（对照可推 3-4.5 GB/s）测得 `e5ad72c` 时 lite 大体仅为对照的 55-86%，并剖析主因是发送路径每响应 CPU 过高；`a58cf18`（record 填充 + DATA 批量化）正是针对该路径。本次同机复测 lite 独涨而对照零变化，方向与幅度（缺口 → 2-3× 优势）闭环了该修复的有效性。
- **绝对值口径**：本机 h2load/回环整链路上限较 #4 主机低（#3 全场 ≤0.8 GB/s、本次 lite 到 1.74 GB/s），故 lite 的绝对天花板在本机仍可能被压测链路封顶；但同一负载发生器下三代理相对对比有效。

### 3.3 HTTP/3（http3_benchmark_client 16 连接×4 流，20s+3s 预热，6 样本中位；OpenResty 无 H3）

| 场景 | 代理 | RPS 中位 | 快档 | Δ vs #3(中位) | p50 ms | p90 ms | p99 ms | 丢包 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **93,463** | 106-109k | +6% | 0.62 | 0.99 | 1.32 | 0 |
| GET 1 KiB | nginx(quic) | 61,552 | 74-77k | +20% | 0.78 | 1.53 | 1.99 | 0 |
| GET 64 KiB | **lite-nginx** | **13,203** | ~19.5k | +76% | 4.62 | 6.18 | 8.24 | 0 |
| GET 64 KiB | nginx(quic) | 8,756 | ~8.8k | -3% | 5.57 | 11.28 | 14.80 | 0 |
| GET 1 MiB | **lite-nginx** | **1,473** | ~1510 | +138% | 35.58 | 67.84 | 84.10 | 0 |
| GET 1 MiB | nginx(quic) | 616 | ~620 | -0% | 80.00 | 161.28 | 182.02 | 0 |
| POST echo 1 MiB | **lite-nginx** | **751** | ~750 | +189% | 67.58 | 129.79 | 147.97 | 0 |
| POST echo 1 MiB | nginx(quic) | 229 | ~277 | -17% | 186.88 | 448.00 | 483.33 | 0 |

双峰/多档样本逐档列举（**档位 = 命中的 worker 数**，机制见 #4 §1/§4.3：客户端 4 个源端口经 reuseport 哈希分配到 4 worker 中的 2/3/4 个；本报告各档均符合等差量化）：

| 格 | lite-nginx 样本 | nginx(quic) 样本 |
|---|---|---|
| GET 1 KiB | 72.1k, 73.4k, 80.6k, 106.3k, 107.1k, 108.8k（2w/3w） | 47.4k, 48.9k, 49.2k, 73.9k, 76.3k, 77.0k（2w/3w） |
| GET 64 KiB | 13.1k ×4, 19.4k, 19.7k（2w/3w） | 5.8k ×2, 8.7-8.8k ×4（2w/3w） |
| GET 1 MiB | 1000, 1035, 1471, 1475, 1510, 1510（2w/3w） | 401, 407, 612, 619, 624, 819（2w/3w/4w） |
| POST echo | 500, 730, 749, 753, 761, 781（2w/3w） | 180, 180, 184, 274, 276, 280（2w/3w） |

按档位归一的每 worker 吞吐（档值 ÷ worker 数，消除抽奖口径）：

| 格 | lite 每 worker | nginx 每 worker | lite/nginx |
|---|---:|---:|---:|
| GET 1 KiB | ~36k | ~25k | **1.44×** |
| GET 64 KiB | ~6.5k | ~2.9k | **2.2×** |
| GET 1 MiB | ~505 | ~205 | **2.5×** |
| POST echo | ~250 | ~92 | **2.7×** |

**要点**

- **大体优势真实建立且口径公平**：#3 的"快档持平"是 `a58cf18` 之前的真实状态；本次 lite 每 worker 2.2-2.7×，且 lite 的 1M/POST 3-worker 档占多数（4/6、5/6），中位数已落在快档。nginx 侧各档与 #3 完全一致（中位变化 -3%~+20% 均在档位抽样噪声内）。
- **GET 1 KiB**：本次 lite 也呈 2w/3w 双档（#3 时单档 88k，即当时 6 样本可能全部抽中同档）。归一后 lite 每 worker +44%（#3 档对档仅 +6%）。
- **延迟**：lite 大体三格 p50/p90/p99 全面优于 nginx（1M p50 35.6ms vs 80.0ms；POST p99 148ms vs 483ms）。
- 6 样本 × 2 代理 × 4 场景全程 0 丢包、0 失败。

---

## 4. 与 #3/#4 的逐项对照结论

1. **新增的优势（本次主结论）**：H2 四场景全面反超（大体 2.0-3.0×、小请求 1.47×）；H3 每 worker 2.2-2.7×。归因 `a58cf18`——直接证据是同机同配置下两对照零变化而 lite 独涨，且恰好在 #4 异机暴露的发送路径 CPU 缺口（40-45%）之后合入并将其翻转为优势。
2. **作废的结论**：#2/#3 "H2 大体 harness-bound（三者同窄区间）"——系 lite 发送路径未打包所致，非负载发生器上限（#4 亦持此判断）。
3. **修正的口径**：H3 双峰不再是"成因未定位"——#4 已定位为 reuseport 源端口哈希的 worker 抽奖，本次两代理各档全部符合等差量化，报告改用"每 worker"口径比较。
4. **保持的优势**：H1 POST echo 2.55×（对照为缓冲配置；流式对照下 ~1.2-1.9×，见 #4 附加实验）。
5. **持续的劣势**：H1 GET 1K 落后 ~38%（130k vs 210k）；lite 此格 p99 6.19ms 劣于对照 ~2ms（跨 #3/#4/#5 的遗留排查项）。
6. **重构无回退**：期间 57 提交（含 IntrusiveList 重设计、poller 指针化、QUIC endpoint 生命周期重构），H1/H3 对照面与 lite 全部格无负向变化（H1 各格 -8%~+11% 均在方差内）。

## 5. 局限（在 #3 基础上更新）

1. 单机同置、绝对值偏低估——不变（相对对比有效）。
2. H3 档位抽样仍随机（6 样本未覆盖全部档组合；lite 未观测到 4-worker 档，与 #4 的 pacing 欠饱和观察一致），已用每 worker 归一口径缓解。
3. `wmem_max=208KB` 限制不变，H3 并发维持 16×4 保守值；H2/H3 更高并发下的优势幅度未测。
4. 本机 H2 大体绝对值可能仍被压测链路封顶（#4 主机同配置 h2load 可推 4.5 GB/s）；lite 真实上限待异机复测。
5. 本次发生 1 起 harness 竞态：h1 nginx 样本 2 因上一轮 nginx 停止后端口（38080）未及时释放而启动失败，4 次测量无效（解析器按无 rps 自动剔除），事后补测该样本（215.1k/89.6k/10.4k/2.16k，与另两样本一致）。改进建议：STOP 后等待端口真正释放再返回。

## 6. 复现

```bash
# 构建（核对 Release/LTO/libc++/clang-20 与 #3 一致）
cmake --build build --target http_benchmark_backend fiber_app_lite_nginx http3_benchmark_client

# 后端
taskset -c 0-3 ./build/example/http_benchmark_backend 19001 &

# 多样本矩阵（本次新增脚本；run_matrix5/run_h3_5 写 results5/，bench3.sh 带 H3 --json）
bash temp/bench/run_matrix5.sh h1 3   # → temp/bench/results5/s{1,2,3}/
bash temp/bench/run_matrix5.sh h2 2
bash temp/bench/run_h3_5.sh 6

# 汇总（中位数 + Δ vs #3 内嵌基线）
python3 temp/bench/parse_results5.py
```

原始数据：`temp/bench/results5/s1..s6/`（H3 含 `.json` 摘要）；冒烟轮 `temp/bench/sanity5/`。
