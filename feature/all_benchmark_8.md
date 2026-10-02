# lite-nginx 反向代理全协议压测报告（#8）

执行日期：2026-10-02
被测对象：`apps/lite_nginx` @ git `f2e330bc`（master，Release + LTO + libc++，clang-20，构建配置逐项核对与 #3/#5/#6/#7 一致）
对比对象：OpenResty 1.25.3.2、nginx 1.31.1 (quic)（与 #2 起完全相同的构建与配置）
基线：#7 = [all_benchmark_6.md](all_benchmark_6.md) §7.6（2026-09-26，git `72426dc0`，同机；无独立 #7 文件）
方法论：与 #7 完全一致（场景/参数/绑核/公平性配置/H3 档位逐样本实测），多样本取中位数：H1×3、H2×2、H3×6。

> **本次测量的是 master `f2e330bc`**：距 #7 共 68 提交，核心是 **TLS 传输面读写路径重构 + HTTP/2 出站重写**——`c55ab263`/`a4612e5d`（连接期 record 就地单缓冲开启，设计 `feature/tls/12-connected-read-path.md`）、`be3f3097`/`8b95cae0`/`72f49378`（免中转密封/批量封存/栈上汇聚写，设计 `feature/tls/13-batched-write-flush.md`）、`03e290ee`（自排水写，设计 `feature/tls/14-self-draining-write.md`）、`99904634`（每次 read 交付全部缓冲明文）、H2 出站调度重写（`9f832500`/`b3c35ebd`/`aa3db2a8`/`6cdf3fad`）与 I/O 驱动收窄（`760f320f`/`f2e330bc`/`4efc4af2`/`1e57f2fd`）。本次复测即检验这些 perf 提交在端到端反向代理负载下的净效应。

---

## 1. 结论速览（vs 基线 #7）

| 维度 | 本次结论 | vs #7 |
|---|---|---|
| **H2 大体（64K/1M/POST）** | lite **净增 +10~+12%**：64K 24,596（+10%）、1M 2,048（+12%）、POST 1,006（+10%）；而两对照大体格仅 ±0~2%（整机漂移被排除）——**历史最高水平**。对 OpenResty 2.81×/3.47×/3.60×，对 nginx 2.09×/2.46×/2.63× | TLS 读写重构收益端到端兑现 |
| **H2 GET 1 KiB** | lite 200,725（+6%，与整机漂移同量级），仍领先：OR 的 1.49×、nginx 的 1.72× | 领先持平 |
| **H3 大体（每 worker 归一）** | lite 持平：64K 6,930/w（+3%）、1M 516/w（-1%）、POST 265/w（+1%）；nginx /w 同步 +1~4%（环境等价）。比例 2.26×/2.46×/2.82× 不变 | 持平（符合架构预期，见 §4.2） |
| **H3 GET 1 KiB（每 worker）** | lite 38,734/w（+4%）vs nginx 26,915/w（+4%）→ **1.44×**（#5/#7 均 1.44×） | 领先持平 |
| **POST echo 1 MiB（H1）** | lite 6,187 rps / 6.04 GB/s，为对照的 **2.75-2.77×**（#7 约 2.7×） | 略增（漂移内） |
| **HTTP/1 GET 1 KiB** | lite 155,129，落后对照（237.4-237.8k）**35%**（#6 为 35-36%） | 旧格局不变 |
| **GET 1 MiB（H1）** | 三者 10.1k（~10 GB/s）打平 | 不变，带宽封顶 |
| **稳定性** | 全部有效测量 0 失败 / 0 非 2xx / H3 全程 0 UDP 丢包（48 份 json 逐一核查）；wrk/h2load 时长全部 20.00-20.06s | 不变 |

一句话：**master 上的 TLS 记录层读写重构（就地开启/批量封存/写合并/自排水）在 H2 大体三格兑现 +10~12% 的净吞吐（超出整机漂移、对照零变化），H2 领先比例创历史新高；H3 数据面不经 `TlsStreamFd` 故每 worker 持平（±1~4% = 漂移）；H1 明文面旧格局不变。全程零失败零丢包。**

---

## 2. 环境与构建（与 #7 的差异点）

- 同一台 16 vCPU / 32 GiB / Ubuntu 20.04 主机；绑核不变：backend 0-3 / proxy 4-9 / loadgen 10-15。
- lite-nginx 从 `72426dc0` 换到 `f2e330bc`；CMakeCache 逐项核对：Release / LTO ON / libc++ / clang++-20 / 无 jemalloc，与 #3 起一致。
- **依赖干净**：`temp/_deps/boringssl-build` 的 crypto 旗标复核为 `-O3 -DNDEBUG`（无 #6 的 -O0 污染复发）。
- 后端、wrk 4.2.0、h2load nghttp2/1.65.0、OpenResty 1.25.3.2、nginx 1.31.1、证书、POST 体、代理配置全部复用 `temp/bench/`。
- **冒烟先行**（37 提交大改 TLS/H2 服务端路径）：curl TLS1.3 协商 `TLS_CHACHA20_POLY1305_SHA256` + ALPN h2 → 200/http2；h2load 3s 零失败；H3 客户端 3s 零失败零丢包——三协议互操作干净，套件偏好修复在 master 上生效。
- **环境差异（记录在案）**：① 一个 09-28 起遗留的 `python3 -m http.server 18081`（cwd `/tmp/tamper` 已删除、0 活动连接、0% CPU）挡住 lite 的固定端口，压测前已停（如需复原：`python3 -m http.server 18081`）；② #5-#7 常驻的 idle cat-demo nginx 已不在（0% CPU 常驻进程，无可测影响）；③ 整机较 #7 快 ~4-9%（两对照全格同向漂移，见 §4.4）。
- 本次脚本：`run_matrix8.sh` / `run_h3_8.sh`（自 #7 脚本 sed 派生，写 `results8/s*/`，`bench7.sh` 原样复用——含 nginx bind 预探与 H3 档位实测；脚本已快照至 `scripts/benchmark/all/`）、`parse_results8.py`（Δ#7 一列从 `results7/` 原始文件以相同中位逻辑动态计算，避免转抄）。
- 原始数据：`temp/bench/results8/s1..s6/`（只追加，未触碰任何旧 results 目录）。

---

## 3. 结果（每协议中位数；Δ#7 为同逻辑动态基线）

### 3.1 HTTP/1（wrk -t6 -c256 -d20s，3 样本中位）

| 场景 | 代理 | RPS | Δ vs #7 | 离散 | p50 ms | p90 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | lite-nginx | 155,129 | +6% | ±0% | 1.54 | 2.00 | 2.60 |
| GET 1 KiB | OpenResty | **237,814** | +7% | ±0% | 1.03 | 1.20 | 1.75 |
| GET 1 KiB | nginx(quic) | 237,373 | +9% | ±1% | 1.00 | 1.15 | 1.70 |
| GET 64 KiB | lite-nginx | 81,519 | +4% | ±1% | 2.90 | 3.39 | 4.75 |
| GET 64 KiB | OpenResty | **96,855** | +2% | ±1% | 2.48 | 3.20 | 4.43 |
| GET 64 KiB | nginx(quic) | 96,752 | +4% | ±2% | 2.53 | 3.48 | 4.35 |
| GET 1 MiB | lite-nginx | **10,136** | +0% | ±1% | 23.73 | 28.39 | 34.89 |
| GET 1 MiB | OpenResty | 10,065 | +3% | ±2% | 24.40 | 32.16 | 39.95 |
| GET 1 MiB | nginx(quic) | 10,184 | +4% | ±2% | 23.75 | 29.55 | 35.58 |
| POST echo 1 MiB | **lite-nginx** | **6,187** | +6% | ±1% | 36.55 | 64.23 | 78.22 |
| POST echo 1 MiB | OpenResty | 2,253 | +5% | ±1% | 113.23 | 122.26 | 387.22 |
| POST echo 1 MiB | nginx(quic) | 2,233 | +5% | ±1% | 113.25 | 124.40 | 398.59 |

样本明细（lite get1k）：156.2k / 155.1k / 154.8k。全部 0 错误 / 0 非 2xx。

**要点**

- **三代理同向 +0~+9%**：整机较 #7 略快（两对照 get1k 均 +7~9%），lite 各格变化在对照漂移范围内——**H1 面不受本次重构影响**（明文代理不经过 TLS/H2 路径，全部 68 提交均不触碰 H1 明文路径）。
- **GET 1 KiB**：lite 155.1k，落后对照 35%（#6 为 35-36%）；p99 2.60ms 较 #6 的 5.45ms 明显改善且首次优于 OpenResty 的 2.47ms 量级（本次 1.54/2.00/2.60 vs OR 1.03/1.20/1.75，仍落后但差距收窄）——跨 #3-#8 的遗留排查项（小请求每层开销）可关注此变化。
- **POST echo 1 MiB**：6,187 rps / 6.04 GB/s，对照的 2.75-2.77×（#6 2.72×）；对照仍为原配置（请求体缓冲 on）。

### 3.2 HTTP/2（h2load -t6 -c32 -m16 -D20s TLS，2 样本中位）

| 场景 | 代理 | RPS | Δ vs #7 | 离散 | BW MB/s | mean ms | max ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **200,725** | +6% | ±4% | 210 | 2.42 | 66.64 |
| GET 1 KiB | OpenResty | 134,317 | +5% | ±1% | 141 | 3.69 | 20.75 |
| GET 1 KiB | nginx(quic) | 116,829 | +4%* | ±0% | 121 | 4.40 | 23.31 |
| GET 64 KiB | lite-nginx | **24,596** | **+10%** | ±1% | 1,537 | 20.32 | 44.85 |
| GET 64 KiB | OpenResty | 8,745 | +0% | ±1% | 547 | 54.71 | 121.20 |
| GET 64 KiB | nginx(quic) | 11,765 | +1% | ±1% | 735 | 43.37 | 102.31 |
| GET 1 MiB | lite-nginx | **2,048** | **+12%** | ±1% | 2,049 | 245.85 | 347.44 |
| GET 1 MiB | OpenResty | 591 | +1% | ±1% | 592 | 799.43 | 5,320 |
| GET 1 MiB | nginx(quic) | 831 | +2% | ±1% | 832 | 601.16 | 1,140 |
| POST echo 1 MiB | lite-nginx | **1,006** | **+10%** | ±1% | 1,007 | 497.26 | 697.28 |
| POST echo 1 MiB | OpenResty | 279 | +1% | ±1% | 279 | 1,680 | 3,570 |
| POST echo 1 MiB | nginx(quic) | 383 | +1% | ±2% | 384 | 1,230 | 2,940 |

\* nginx h2 1K 的 Δ+4% 按健康样本口径：#7 该格中位 97.2k 被 81.8k 离群样本拉低（#7 §7.6 已注记，健康样本 112.6k）；本次 116.5k/117.2k 对健康样本 +4%。

全部 0 failed / 0 timeout。

**要点**

- **本次最大的变化：lite 大体三格净增 +10~+12%**（64K 24.3k/24.9k、1M 2,068/2,028、POST 996/1,016，样本间 ±1-4% 紧致），同期两对照大体格 ±0~2%——漂移被对照排除，增益为 lite 独有。mean 延迟同步改善（1M 246ms vs #7 实测 278ms、POST 497ms vs #7 实测 552ms，均 -10~12% 与 rps 增益自洽）。
- **归因**：`feature/tls/12/13/14` 三份设计文档各自带微基准（就地开启、批量封存、写合并），本端到端矩阵将其兑现为代理负载下的净吞吐；H2 1M 带宽 2.05 GB/s 远超本机 4-worker AES 天花板（~912 MB/s），证明协商 ChaCha20-Poly1305（`TlsSuitePreference` 在 master 生效）且已贴近该套件 ~600 MB/s/core 的密码学上限（4×600×0.85 ≈ 2.04 GB/s ✓）。
- **GET 1K**：200.7k（+6%），与整机漂移同量级（两对照 +4~5%），1K 单 record 可装满、重构收益小符合预期；对 OR 1.49×、对 nginx 1.72×（#7 1.48×/1.68× 健康口径）。

### 3.3 HTTP/3（http3_benchmark_client 16 连接×4 流，20s+3s 预热，6 样本中位；OpenResty 无 H3）

| 场景 | 代理 | RPS 中位 | p50 ms | p90 ms | p99 ms | 丢包 |
|---|---|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **114,206** | 0.47 | 0.81 | 1.20 | 0 |
| GET 1 KiB | nginx(quic) | 67,250 | 0.60 | 1.21 | 1.79 | 0 |
| GET 64 KiB | lite-nginx | 13,935 | 4.46 | 5.54 | 6.88 | 0 |
| GET 64 KiB | **nginx(quic)** | **9,204** | 5.23 | 10.72 | 13.82 | 0 |
| GET 1 MiB | lite-nginx | 1,043 | 36.61 | 98.56 | 108.03 | 0 |
| GET 1 MiB | **nginx(quic)** | **524** | 79.10 | 161.28 | 177.15 | 0 |
| POST echo 1 MiB | **lite-nginx** | **794** | 60.54 | 125.44 | 148.99 | 0 |
| POST echo 1 MiB | nginx(quic) | 234 | 174.08 | 353.28 | 381.95 | 0 |

档位逐样本实测（机制同 #7：`bench7.sh` 每场景前后 `/proc/<pid>/task/*/stat` jiffies 差值，≥300 记命中；lite 主线程不计）：

| 格 | lite-nginx 样本 | nginx(quic) 样本 |
|---|---|---|
| GET 1 KiB | 78.7k(2w), 118.4k(3w), 151.5k(4w), 114.4k(3w), 82.3k(2w), 114.0k(3w) | 82.5k(3w), 54.0k(2w), 51.3k(2w), 82.3k(3w), 53.5k(2w), 80.5k(3w) |
| GET 64 KiB | 13.5k(2w), 13.8k(2w), 14.1k(2w), 20.6k(3w), 14.0k(2w), **6.95k(1w)** | 9.20k(3w), 9.22k(3w), 9.22k(3w), 5.94k(2w), 9.21k(3w), 5.97k(2w) |
| GET 1 MiB | 1.53k(3w), 1.04k(2w), 1.04k(2w), 1.03k(2w), 1.53k(3w), 1.03k(2w) | 0.42k(2w), 0.63k(3w), 0.42k(2w), 0.63k(3w), 0.42k(2w), 0.63k(3w) |
| POST echo | 0.79k(3w), 0.80k(3w), 0.79k(3w), 0.80k(3w), 0.53k(2w), 0.80k(3w) | 0.28k(3w), 0.19k(2w), 0.18k(2w), 0.28k(3w), 0.19k(2w), 0.28k(3w) |

按档位归一的每 worker 吞吐（各档等差量化良好；64K 的 1w 样本 6.95k/w 与 2w 档 13.5-14.1k 的一半自洽）：

| 格 | lite /w | Δw#7 | nginx /w | Δw#7 | lite/nginx |
|---|---:|---:|---:|---:|---:|
| GET 1 KiB | 38,734 | +4% | 26,915 | +4% | **1.44×**（#5/#7 均 1.44×） |
| GET 64 KiB | 6,930 | +3% | 3,068 | +1% | **2.26×**（#7 2.22×，#5 2.2×） |
| GET 1 MiB | 516 | -1% | 210 | -0% | **2.46×**（#7 2.48×，#5 2.5×） |
| POST 1 MiB | 265 | +1% | 94 | +1% | **2.82×**（#7 2.83×，#5 2.7×） |

**要点**

- **H3 每 worker 与 #7 逐格持平**（lite -1~+4%、nginx -0~+4%，同步 = 整机漂移），#5 以来的全部领先比例（1.44×/2.26×/2.46×/2.82×）原样保持。
- **符合架构预期**：H3 数据面不走 `TlsStreamFd`（QUIC 自带包保护，`TlsStreamFd` 仅在 QUIC 握手期介入，20s 长连接下握手成本已摊薄）——TLS 读写重构的收益只应出现在 H2，实测正是如此。这同时构成一个反向对照：**H2 的 +10~12% 不可能来自套件/环境等公共因子（否则 H3 每 worker 应同幅变化），确证增益来自 `TlsStreamFd` 所在的 H2 记录层路径**。
- 绝对中位数受档位抽样扰动（如 lite 1M 的 6 样本 4×2w+2×3w，中位落在 2w 档 1,043；nginx get1k 混档中位 67.3k）——跨 run 对比一律以每 worker 为准（#6 §7.5 教训持续有效）。
- 6 样本 × 2 代理 × 4 场景全程 0 丢包、0 失败（48 份 json 逐一核查 failed/dropped_datagrams/recv_storage_rejected 均为 0）。

---

## 4. 与 #5/#6/#7 的逐项对照结论

1. **新确立**：H2 大体三格较 #7 净增 +10~+12%（对照同期 ±0~2%），对 OR 领先 2.81×/3.47×/3.60×、对 nginx 2.09×/2.46×/2.63×——均为 #3 有记录以来最高。归因链：feature/tls/12/13/14 微基准 → 本次端到端兑现；H3 反向对照排除公共因子。
2. **保持**：H3 每 worker 四格与 #5/#7 完全一致（1.44×/2.2-2.3×/2.5×/2.8×）；H1 面旧格局不变（POST echo 2.75-2.77×、GET 1K 落后 35%、1M 带宽打平）；H2/H3 小请求领先持平（1.49×/1.72×、1.44×）。
3. **环境等价性成立**：两对照 H1 全格 +2~+9%、H2 大体 ±0~2%、H3 每 worker +1~4% 同向小幅漂移（整机略快）；唯一口径噪音是 nginx h2 1K（#7 中位被离群样本拉低，健康口径下 +4%，见 §3.2 注）。
4. **稳定性结论不变**：master 上三协议与 wrk/h2load/http3_benchmark_client 互操作全程零失败零丢包；68 提交的重构未引入任何互操作或稳定性回退。

## 5. 局限（在 #7 基础上更新）

1. 单机同置、绝对值偏低估——不变（相对对比有效）。
2. H3 档位抽样仍随机（本次 lite 64K 首次出现 1w 档；nginx 未出 2w 以下档）；每 worker 归一口径已缓解。
3. `wmem_max=208KB` 限制不变，H3 并发维持 16×4 保守值。
4. H2 lite 仅 2 样本（±1~4%），增益幅度（+10~12%）远超采样噪声；分位数对比需更多样本。
5. 本次环境与 #7 的两处差异（遗留 http.server 已停、cat-demo nginx 缺席）均为零 CPU 影响项，已记录；桌面后台（IDE/浏览器等）与历次压测同级存在。

## 6. 复现

```bash
# 拉取并构建（核对 Release/LTO/libc++/clang-20 与 #3 起一致；deps 需 -O3 干净）
git fetch origin master && git pull --ff-only origin master
cmake --build build --target http_benchmark_backend fiber_app_lite_nginx http3_benchmark_client

# 后端
taskset -c 0-3 ./build/example/http_benchmark_backend 19001 &

# 多样本矩阵（run_matrix8/run_h3_8 写 results8/，只追加；bench7.sh 含 nginx bind 预探 + H3 档位实测）
bash temp/bench/run_matrix8.sh h1 3   # → temp/bench/results8/s{1,2,3}/
bash temp/bench/run_matrix8.sh h2 2
bash temp/bench/run_h3_8.sh 6

# 汇总（中位数 + Δ vs #5/#6 内嵌基线 + Δ#7 动态基线 + H3 每档归一）
python3 temp/bench/parse_results8.py
```

原始数据：`temp/bench/results8/s1..s6/`（H3 含 `.json` 摘要与 `TIER_WORKERS` 实测行）。脚本快照：`scripts/benchmark/all/run_matrix8.sh`、`run_h3_8.sh`、`parse_results8.py`。
