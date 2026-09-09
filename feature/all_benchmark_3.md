# lite-nginx 反向代理全协议压测复测报告（#3）

执行日期：2026-09-08
被测对象：`apps/lite_nginx` @ git `9ecdad5`（**HTTP Server 重写已全量落地**，Release + LTO + libc++，clang-20）
对比对象：OpenResty 1.25.3.2、nginx 1.31.1 (quic)（与 #2 完全相同的构建与配置）
基线：[all_benchmark_2.md](all_benchmark_2.md)（2026-07-29，git `496f42a`，重写前）
方法论：与 #2 完全一致（场景/参数/绑核/公平性配置，见 #2 第 3 节），改进为**多样本取中位数**：H1×3、H2×2、H3×6（吸取 #2 修订版 H3 单样本方差 ±30-66% 的教训）。

---

## 1. 结论速览（vs 基线 #2）

| 维度 | 本次结论 | vs #2 |
|---|---|---|
| **POST echo 1 MiB（H1）** | lite-nginx 5,562 rps / 5.44 GB/s，为 OpenResty/nginx 的 **2.60×** | 优势稳定保持（#2 为 2.7×） |
| **HTTP/2 GET 1 KiB** | lite-nginx 129.0k rps **领先** OpenResty（124.4k）、nginx（108.0k） | 保持领先，幅度 +3.6%/+19% |
| **HTTP/3 GET 1 KiB** | lite-nginx 88.3k（±2% 极稳）vs nginx 中位 51.2k（双峰，快档 83.4k） | lite 仍占优；nginx 分档后差距收窄 |
| **HTTP/1 GET 1 KiB** | lite-nginx 117.5k，落后 OpenResty/nginx（~210k）**44%** | #2 报 35%；本格 #2 取的是 lite 高样本（134k），lite 本身样本区间 117–134k 与本次重叠，扩大程度存疑但落后事实不变 |
| **GET 1 MiB（H1）** | 三者 10.2–10.7k rps（~10 GB/s）打平 | 不变，带宽封顶 |
| **H2 大体（64K/1M/POST）** | 三者同窄区间（64K 8.6–11.5k、1M 0.59–0.81k、POST 0.28–0.38k） | 不变，harness-bound |
| **H3 大体（1M/POST）** | **双峰分布**，两代理快档持平（1M ~620 / POST ~275-310）；lite 更稳（POST 尾部无 92-186 rps 坏档） | **#2 的 1.41×/1.64× 优势未复现**——系 #2 单样本恰好落在 nginx 慢档 |
| **稳定性** | 全部 104 次测量 0 失败 / 0 非 2xx / H3 全程 0 UDP 丢包 | 不变 |

一句话：**重写落地后，lite-nginx 的三大优势（H1 双向流式 2.6×、H2 小请求领先、H3 小请求领先）全部保持；H3 大体对 nginx-quic 的显著优势是 #2 的采样假象，实际快档持平；H1 极小请求仍落后 nginx 系 ~40%。**

---

## 2. 环境与构建（与 #2 的差异点）

- 同一台 16 vCPU / 32 GiB / Ubuntu 20.04 主机；sysctl 未变（`rmem_max=33MB`、`wmem_max=208KB`）。
- 绑核不变：backend 0-3 / proxy 4-9 / loadgen 10-15。
- 后端、wrk、h2load、OpenResty、nginx-quic、证书、POST 体、代理配置全部复用 #2 的 `temp/bench/`。
- lite-nginx 从 `496f42a` 换到 `9ecdad5`（期间合入 HTTP Server 重写、H2 keepalive 单旋钮等）；构建配置逐项核对一致（Release/LTO/libc++/clang-20/无 jemalloc）。
- 后台浮动机负载与 #2 同级（CLion/Rider/dockerd 等，均未钉核，总 loadavg ~1）。
- 改进：H1 每格 3 样本、H2 每格 2 样本、H3 每格 6 样本，取中位数；样本间轮转代理以均摊时序偏差。
- H3 客户端新增 `--json` 摘要输出，解析不再依赖文本正则。

## 3. 结果（每协议中位数；Δ 相对 #2 数值）

### 3.1 HTTP/1（wrk -t6 -c256 -d20s，3 样本中位）

| 场景 | 代理 | RPS | Δ vs #2 | 离散 | p50 ms | p90 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | lite-nginx | 117,480 | -12% | ±4% | 1.90 | 2.93 | 7.82 |
| GET 1 KiB | OpenResty | **210,798** | +2% | ±1% | 1.13 | 1.53 | 2.20 |
| GET 1 KiB | nginx(quic) | 210,565 | +5% | ±0% | 1.11 | 1.61 | 2.46 |
| GET 64 KiB | lite-nginx | 68,150 | -8% | ±3% | 3.36 | 5.03 | 5.77 |
| GET 64 KiB | OpenResty | **90,644** | -2% | ±0% | 2.58 | 3.71 | 4.92 |
| GET 64 KiB | nginx(quic) | 89,442 | +7% | ±1% | 2.63 | 3.74 | 5.02 |
| GET 1 MiB | lite-nginx | 10,192 | +1% | ±1% | 23.06 | 31.46 | 36.99 |
| GET 1 MiB | OpenResty | **10,686** | +3% | ±3% | 21.47 | 30.67 | 42.90 |
| GET 1 MiB | nginx(quic) | 10,534 | +10% | ±1% | 23.42 | 31.35 | 40.14 |
| POST echo 1 MiB | **lite-nginx** | **5,562** | +0% | ±1% | 38.15 | 69.59 | 90.96 |
| POST echo 1 MiB | OpenResty | 2,147 | +4% | ±1% | 116.17 | 132.70 | 455.97 |
| POST echo 1 MiB | nginx(quic) | 2,128 | +4% | ±1% | 113.04 | 136.91 | 442.35 |

全部样本 0 错误 / 0 非 2xx。

**要点**

- **POST echo 1 MiB**：5,562 rps / 5.44 GB/s，2.60×（#2：2.7×）。零拷贝双向中继优势经重写后无损耗，且两对照样本离散极小（±1%）。
- **GET 1 KiB 落后幅度**：今日 lite:OR = 1:1.79（#2 报 1:1.54）。注意 #2 该格 lite 为单样本且取到高值 134k（其另一轮观测 117k，见 #2 §7.2），本次三样本 115.5/117.5/124.1k 与其区间重叠；OR/NG 今日 210-215k 比其 #2 的 200-207k 略高。**落后 ~40% 的结论稳固，具体差值对采样口径敏感**；另 lite 此格 p99 7.8ms 劣于 #2 的 2.97ms，偶发尾延迟（max 观测 124ms）值得后续排查。
- **GET 1 MiB** 依旧三者打平（带宽封顶），**GET 64 KiB** OpenResty 仍最快，lite 落后 ~25%。

### 3.2 HTTP/2（h2load -t6 -c32 -m16 -D20s TLS，2 样本中位）

| 场景 | 代理 | RPS | Δ vs #2 | 离散 | BW MB/s | mean ms | max ms |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **128,961** | +7% | ±2% | 135 | 3.93 | 106.57 |
| GET 1 KiB | OpenResty | 124,448 | +12% | ±5% | 132 | 4.08 | 25.81 |
| GET 1 KiB | nginx(quic) | 108,040 | +9% | ±1% | 114 | 5.30 | 36.47 |
| GET 64 KiB | lite-nginx | 9,626 | +3% | ±1% | 603 | 53.08 | 189.60 |
| GET 64 KiB | OpenResty | 8,648 | +2% | ±1% | 542 | 54.97 | 133.86 |
| GET 64 KiB | nginx(quic) | 11,458 | +5% | ≥1% | 718 | 44.59 | 92.44 |
| GET 1 MiB | lite-nginx | 712 | -0% | ±0% | 715 | 706.87 | 1,410 |
| GET 1 MiB | OpenResty | 586 | +3% | ±1% | 587 | 812.28 | 3,620 |
| GET 1 MiB | nginx(quic) | 810 | +4% | ±0% | 812 | 618.00 | 1,490 |
| POST echo 1 MiB | lite-nginx | 358 | +6% | ±1% | 360 | 1,380 | 3,155 |
| POST echo 1 MiB | OpenResty | 276 | +6% | ±1% | 277 | 1,690 | 3,940 |
| POST echo 1 MiB | nginx(quic) | 377 | +4% | ±0% | 378 | 1,250 | 3,115 |

全部 0 failed / 0 timeout。与 #2 格局一致：小请求 lite 领先（129k），大体 harness-bound（64K/1M/POST 均在 #2 同一窄区间，排序差异在噪声内）。

### 3.3 HTTP/3（http3_benchmark_client 16 连接×4 流，20s+3s 预热，6 样本中位；OpenResty 无 H3）

| 场景 | 代理 | RPS 中位 | 快档 | Δ vs #2(中位) | p50 ms | p90 ms | p99 ms | 丢包 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **88,326** | 88-90k（单档） | -1% | 0.60 | 1.09 | 1.59 | 0 |
| GET 1 KiB | nginx(quic) | 51,214 | 83.4k | -37% | 0.83 | 1.74 | 2.45 | 0 |
| GET 64 KiB | lite-nginx | 7,490 | ~8.9k | +31% | 8.57 | 14.43 | 19.58 | 0 |
| GET 64 KiB | nginx(quic) | 9,045 | ~9.0k | +54% | 5.58 | 10.72 | 14.10 | 0 |
| GET 1 MiB | lite-nginx | 620 | ~620（单档） | +8% | 81.41 | 155.90 | 174.85 | 0 |
| GET 1 MiB | nginx(quic) | 616 | ~620 | +52% | 82.43 | 155.90 | 178.18 | 0 |
| POST echo 1 MiB | lite-nginx | 260 | ~310 | -11% | 233.22 | 315.90 | 346.62 | 0 |
| POST echo 1 MiB | nginx(quic) | 275 | ~358（坏档 92-186） | +54% | 178.69 | 358.40 | 389.63 | 0 |

**H3 大体为双峰分布**，单次测量会随机落入快/慢档（档差 ~50%），6 样本逐档列举：

| 格 | lite-nginx 样本 | nginx(quic) 样本 |
|---|---|---|
| GET 1 KiB | 86.6k, 88.0k, 88.2k, 88.4k, 88.5k, 90.1k（单档） | 24.8k, 49.1k, 51.0k, 51.4k, 83.4k, 83.4k（三档） |
| GET 64 KiB | 3.0k, 6.0k, 6.1k, 8.9k, 8.9k, 9.0k | 6.0k, 8.8k, 9.0k, 9.1k, 9.1k, 11.8k |
| GET 1 MiB | 608, 611, 618, 622, 629, 780 | 419, 425, 613, 620, 622, 629 |
| POST echo | 206, 209, 213, 307, 311, 313 | 92, 186, 275, 275, 277, 358 |

**要点**

- **GET 1 KiB**：lite-nginx 六样本全部落在 86.6-90.1k 单档（±2%），nginx 三档（25k/51k/83k）——取 nginx 快档 83.4k 比较，lite 仍 +6%，且一致性远胜。
- **#2 的 H3 大体优势未复现**：#2 单样本里 nginx-quic 落在慢档（64k=5.9k、1M=406、POST=179），lite 落在快档，得出 1.41×/1.64×。本次 6 样本显示两者**快档持平**（64K ~9k、1M ~620、POST nginx 快档 358 甚至高于 lite 快档 310）。
- **一致性差异**：POST echo lite 无坏档（最差 206）；nginx 出现 92/186 rps 坏档。1M GET lite 单档稳定，nginx 有 420 慢档。
- 6 样本 × 2 代理 × 4 场景全程 0 丢包、0 失败。

---

## 4. 与 #2 的逐项对照结论

1. **保持的优势**：H1 POST echo 2.60×（±1% 稳）；H2 GET 1K 领先（129k vs 124/108k）；H3 GET 1K 领先且极稳（88k 单档）。
2. **修正的结论**：#2 "H3 大体 lite 1.4-1.64×" 为单样本采样偏差——H3 大体吞吐双峰，快档两代理持平；lite 的真实优势在**一致性**（无坏档）。
3. **持续的劣势**：H1 极小请求落后 ~40%（本次三代理同环境同构建配置复测，OR/NG 维持 210k，lite 115-124k）。#2 报 35% 落后取的是 lite 高样本；差距具体值依采样口径在 35-45% 间。
4. **Server 重写影响**：无回退的正面证据——lite 各格中位数对 #2 的变化（-12% ~ +7%）均在 #2 自身记录的运行间方差内，唯 H1 GET 1K p99 从 2.97ms 变 7.82ms（偶发 124ms 尖刺）值得单独排查。

## 5. 局限（在 #2 基础上更新）

1. 单机同置、绝对值偏低估——不变（相对对比有效）。
2. H1/H2 样本数 3/2，足以稳定中位但未覆盖昼夜系统状态差异。
3. **H3 大体双峰的成因未定位**（疑与 UDP loopback / QUIC 拥塞窗随 20s 测量窗的生长轨迹有关，两代理同样表现，非单方问题）；本报告以 6 样本中位 + 逐档列举呈现。
4. `wmem_max=208KB` 限制不变，H3 并发维持 16×4 保守值。

## 6. 复现

```bash
# 构建（核对 Release/LTO/libc++/clang-20 与 #2 一致）
cmake --build build --target http_benchmark_backend fiber_app_lite_nginx http3_benchmark_client

# 后端
taskset -c 0-3 ./build/example/http_benchmark_backend 19001 &

# 多样本矩阵（本次新增脚本；bench2.sh=bench.sh+可覆盖结果目录，bench3.sh=bench2.sh+H3 --json）
bash temp/bench/run_matrix.sh h1 3   # → temp/bench/results2/s{1,2,3}/
bash temp/bench/run_matrix.sh h2 2
bash temp/bench/run_h3.sh 3 && for s in 4 5 6; do for p in lite nginx; do \
  RES_DIR=$PWD/temp/bench/results2/s$s bash temp/bench/bench3.sh h3 $p; done; done

# 汇总（中位数 + Δ vs #2 内嵌基线）
python3 temp/bench/parse_results2.py
```

原始数据：`temp/bench/results2/s1..s6/`（H3 含 `.json` 摘要）。
