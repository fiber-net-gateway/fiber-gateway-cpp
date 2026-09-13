# lite-nginx 反向代理全协议压测复测报告（#4）

执行日期：2026-09-12
被测对象：`apps/lite_nginx` @ git `e5ad72c`（Release + ThinLTO + 静态 libc++，clang-22）
对比对象：OpenResty 1.25.3.2（OpenSSL 1.1.1w）、nginx 1.31.3 (quic, BoringSSL)
基线：[all_benchmark_3.md](all_benchmark_3.md)（2026-09-08，git `9ecdad5`）
方法论：与 #3 一致——场景/参数/绑核/公平性配置沿用 #2 第 3 节，多样本取中位数（H1×3、H2×2、H3×6，样本间轮转代理顺序）。

> **本次不是同机复测。** #2/#3 的 `temp/bench/` 压测环境位于另一台主机（16 vCPU / Ubuntu 20.04 / clang-20），本机不存在；本次在新主机上按 #3 方法论**从零重建**了全部工具链与脚本（第 2 节）。绝对值与 #3 不可直接比较（Δ 列仅供参考），**同机三代理的相对对比有效**。

---

## 1. 结论速览（vs 基线 #3）

| 维度 | 本次结论 | vs #3 |
|---|---|---|
| **POST echo 1 MiB（H1）** | lite-nginx 4,036 rps，为 OpenResty/nginx（3,315 / 3,240）的 **1.22–1.25×** | #3 的 2.60× **系对照组配置差异**：本次 nginx 系开启 `proxy_request_buffering off` 流式转发；改回 nginx 默认（请求体缓冲落盘）后 nginx/OpenResty 为 2,309 / 2,135 rps、p50 105–111 ms，与 #2/#3 数值（2,128–2,147 rps、p50 113–116 ms）吻合，此时 lite 为其 **1.75–1.89×** |
| **HTTP/2 GET 1 KiB** | lite-nginx 175.5k rps **领先** OpenResty（155.7k）、nginx（152.5k）13–15% | 保持领先（#3 为 +3.6%/+19%） |
| **HTTP/2 大体（64K/1M/POST）** | **新发现劣势**：64K 26.6k vs 48.6k（55%）、1M 2.69k vs 4.5k（60%）、POST 1.37k vs 1.6k（86%） | #3 认为"harness-bound 三者同区间"；本机 h2load 可推到 3–4.5 GB/s（#3 仅 0.6–0.8 GB/s），压测器不再是瓶颈后差距显现。第 4 节给出 CPU/系统调用剖析 |
| **HTTP/3 GET 1 KiB** | 中位 nginx-quic 104.0k vs lite 79.5k，但**档位 = 命中的 worker 数**（4 个客户端源端口经 reuseport 哈希抽奖，nginx 53k/97–106k/134k ≈ 2/3/4 worker，lite 73–82k/108k ≈ 2/3 worker）；分发均匀时两者持平（110k/98k vs 107k/106k），lite 轻载 worker 受默认 pacing 影响欠饱和（§4.3） | 表面反转，实为采样口径 + pacing；#3 的 lite 88k 单档 / nginx 三档同理 |
| **HTTP/3 64K / POST** | lite 领先：64K 38.0k vs 35.3k（+8%），POST 1,571 vs 1,389（+13%） | 与 #3 "快档持平"结论方向一致，本次 lite 略优 |
| **HTTP/3 1 MiB** | nginx 3,378 vs lite 2,853（nginx +18%） | #3 持平；本次 nginx 优 |
| **HTTP/1 GET 1 KiB** | lite 124.6k，落后 OpenResty/nginx（188k / 185k）**34%** | 与 #3 "落后 35–45%"一致 |
| **GET 64 KiB（H1）** | lite 66.2k，落后 ~29% | #3 为 ~25%，一致 |
| **GET 1 MiB（H1）** | 三者 8.4–8.9k rps（~8.4–8.9 GB/s）打平 | 不变，带宽封顶（本机 loopback 上限比 #3 低 ~15%） |
| **稳定性** | 108 次正式测量 0 失败 / 0 非 2xx；H3 客户端 0 丢包 | 但 H3 大体场景内核 `UdpRcvbufErrors` 非零（第 3.3 节，本机 `rmem_max` 仅 208 KB） |

一句话：**lite-nginx 保持 H1 双向流式、H2 小请求、H3 64K/POST 的优势，但 #2/#3 报告的 H1 POST 2.6× 有约一半来自对照组的请求体缓冲配置；换到不受压测器限制的主机后，HTTP/2 大体路径暴露出 40–45% 的吞吐差距（每响应 CPU 为 nginx 的 1.5–1.9×，87% 在内核态，见 §4.2），H3 1 KiB 也被 nginx-quic 反超。**

---

## 2. 环境与构建（与 #3 的差异点）

| 项 | #3 | 本次 |
|---|---|---|
| 主机 | 16 vCPU 虚拟机，Ubuntu 20.04 | **WSL2**（Linux 6.6.114），i7-13700H 20 线程（6P+8E，hypervisor 调度，无法真正物理隔离），31 GiB，Ubuntu 24.04 |
| 编译器 | clang-20 | clang-22（lite-nginx：Release + ThinLTO + 静态 libc++，无 jemalloc；nginx-quic 与 OpenResty 同用 clang-22） |
| nginx (quic) | 1.31.1 | **1.31.3**（`temp/nginx-install`，BoringSSL，H2+H3） |
| OpenResty | 1.25.3.2 / 系统 OpenSSL 1.1.1f | 1.25.3.2 / 源码构建 OpenSSL 1.1.1w、PCRE 8.45、`--with-http_v2_module --with-http_ssl_module --with-pcre-jit` |
| wrk / h2load | 4.2.0 / nghttp2 1.65 | 4.2.0（clang-22 + OpenSSL 1.1.1w）/ nghttp2 **1.59**（Ubuntu deb） |
| sysctl | `rmem_max=33 MB`, `wmem_max=208 KB` | **`rmem_max=208 KB`**, `wmem_max=208 KB`（无 sudo，不可调） |
| 绑核 | backend 0-3 / proxy 4-9 / loadgen 10-15 | 相同；16-19 留给后台进程 |
| 后台负载 | CLion/Rider/dockerd | JetBrains remote-dev-server/Rider/dockerd/mysqld，loadavg 起跑前 ~1–4 |
| 代理配置 | 见 #2 §8.3 | 4 worker、上游 keepalive 256、`proxy_buffering off` + 64k 缓冲、`keepalive_requests 1000000`；**新增** `proxy_request_buffering off`（与 lite 原生流式对齐，见 3.1 附加实验） |

### 2.1 本机时钟问题（影响 wrk，已处理）

WSL2 的 `CLOCK_REALTIME` 在正式压测前 4 分钟内被**回拨 7 次（每次 −2.1～−2.4 s）**，之后 100 分钟内零回拨（`clockwatch.py` + 每 5 s 对比 Windows 主机时钟的 `hostclock.py`，1,072 个采样偏移全部在 −0.085～0 s）。wrk 用 `gettimeofday` 计时，受影响样本表现为"20 s 测试报告 17.6 s、`timeout` ≈ 连接数"（在途请求延迟变负被计为超时），rps 虚高 ~13%。h2load / http3_benchmark_client 使用单调时钟不受影响。处理：受影响的 H1 s1（7/12 格）在矩阵结束后**整样本重跑**（19:56–20:00，全部 20.03–20.10 s、0 timeout），原始文件归档于 `temp/bench/results2_s1_clockstep_h1/`；`parse_results2.py` 保留对回拨样本按名义时长重算的逻辑并标注。

## 3. 结果（每协议中位数；Δ 相对 #3 数值，跨主机仅供参考）

### 3.1 HTTP/1（wrk -t6 -c256 -d20s，3 样本中位）

| 场景 | 代理 | RPS | Δ vs #3 | 离散 | BW MB/s | p50 ms | p90 ms | p99 ms | max ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | lite-nginx | 124,595 | +6% | ±1% | 131 | 1.88 | 2.73 | 4.30 | 56.30 |
| GET 1 KiB | **OpenResty** | **188,470** | -11% | ±3% | 215 | 1.26 | 1.84 | 2.68 | 10.64 |
| GET 1 KiB | nginx(quic) | 185,116 | -12% | ±2% | 210 | 1.29 | 1.91 | 3.46 | 15.46 |
| GET 64 KiB | lite-nginx | 66,191 | -3% | ±4% | 4,147 | 3.46 | 5.36 | 7.64 | 59.38 |
| GET 64 KiB | **OpenResty** | **92,730** | +2% | ±3% | 5,806 | 2.55 | 3.80 | 5.18 | 17.22 |
| GET 64 KiB | nginx(quic) | 91,606 | +2% | ±7% | 5,745 | 2.60 | 4.15 | 5.91 | 20.92 |
| GET 1 MiB | lite-nginx | 8,389 | -18% | ±1% | 8,387 | 28.47 | 36.62 | 46.71 | 117.38 |
| GET 1 MiB | **OpenResty** | **8,885** | -17% | ±3% | 8,888 | 27.45 | 36.35 | 47.21 | 75.21 |
| GET 1 MiB | nginx(quic) | 8,873 | -16% | ±1% | 8,878 | 28.78 | 38.05 | 48.69 | 135.43 |
| POST echo 1 MiB | **lite-nginx** | **4,036** | -27% | ±1% | 4,035 | 68.04 | 86.52 | 114.17 | 224.50 |
| POST echo 1 MiB | OpenResty | 3,315 | +54% | ±2% | 3,318 | 74.39 | 88.35 | 118.91 | 810.51 |
| POST echo 1 MiB | nginx(quic) | 3,240 | +52% | ±1% | 3,236 | 76.25 | 90.37 | 120.47 | 847.06 |

全部 36 样本 0 socket 错误 / 0 非 2xx，运行时长 20.03–20.10 s。

**附加实验：对照组请求体缓冲（nginx 默认 `proxy_request_buffering on`，`client_body_buffer_size 16k`）**

| 代理 | H1 POST echo RPS（3 样本中位） | p50 / p90 / p99 ms | H2 POST echo RPS（2 样本中位） |
|---|---:|---|---:|
| OpenResty（请求体缓冲） | 2,135（2,080 / 2,135 / 2,173） | 111.0 / 152.4 / 295.8 | 1,502 |
| nginx(quic)（请求体缓冲） | 2,309（2,276 / 2,309 / 2,309） | 105.4 / 124.5 / 379.8 | 1,484 |
| 对比：流式（主表） | 3,315 / 3,240 | 74–76 / 88–90 / 119–120 | 1,600 / 1,575 |

**要点**

- **POST echo 1 MiB**：流式对流式 lite 为 1.22–1.25×（4,036 vs 3,315/3,240）。把 nginx 系改回默认请求体缓冲后，其 rps / p50（2,135–2,309 / 105–111 ms）与 #2/#3 记录（2,065–2,147 / 113–127 ms）几乎完全一致——**#2 §8.3 列出的对照配置只关闭了响应缓冲，未关闭请求体缓冲，1 MiB 请求体先落 `client_body_temp_path` 再转发，这是 #2/#3 "2.6–2.7×" 的主要来源**。lite 零拷贝双向中继的真实优势约 1.2×（流式对照）～1.8×（默认对照）。
- **GET 1 KiB**：lite:OR = 1:1.51，落后 34%，与 #3 区间（35–45%）一致；p99 4.30 ms 优于 #3 的 7.82 ms，但 max 56 ms 的偶发尖刺仍在（三样本 max 均 >50 ms，nginx 系 10–15 ms）。
- **GET 64 KiB** 落后 ~29%（#3 ~25%）；**GET 1 MiB** 三者打平于本机 loopback 上限 ~8.4–8.9 GB/s。

### 3.2 HTTP/2（h2load -t6 -c32 -m16 -D20s TLS，2 样本中位）

| 场景 | 代理 | RPS | Δ vs #3 | 离散 | BW MB/s | mean ms | max ms |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **175,544** | +36% | ±0% | 183 | 2.87 | 87.07 |
| GET 1 KiB | OpenResty | 155,740 | +25% | ±12% | 146 | 9.12 | 31.56 |
| GET 1 KiB | nginx(quic) | 152,469 | +41% | ±2% | 159 | 3.56 | 22.70 |
| GET 64 KiB | lite-nginx | 26,643 | +177% | ±0% | 1,659 | 19.18 | 81.95 |
| GET 64 KiB | OpenResty | 48,582 | +462% | ±2% | 2,990 | 11.98 | 61.12 |
| GET 64 KiB | **nginx(quic)** | **48,592** | +324% | ±1% | 3,000 | 10.47 | 40.41 |
| GET 1 MiB | lite-nginx | 2,691 | +278% | ±1% | 2,683 | 189.88 | 506.31 |
| GET 1 MiB | OpenResty | 4,467 | +662% | ±0% | 4,454 | 112.08 | 818.28 |
| GET 1 MiB | **nginx(quic)** | **4,513** | +457% | ±1% | 4,465 | 111.97 | 658.07 |
| POST echo 1 MiB | lite-nginx | 1,371 | +283% | ±1% | 1,362 | 371.62 | 801.14 |
| POST echo 1 MiB | **OpenResty** | **1,600** | +480% | ±0% | 1,608 | 318.35 | 544.22 |
| POST echo 1 MiB | nginx(quic) | 1,575 | +318% | ±0% | 1,587 | 322.68 | 661.64 |

全部 0 failed / 0 timeout。

**要点**

- **GET 1 KiB**：lite 175.5k 领先 13–15%，两样本 ±0%，优势比 #3 更明确（OpenResty 两样本 137.7k/173.7k，离散 ±12%）。
- **大体格局改变**：#3 时 h2load 在旧主机上只能推到 0.6–0.8 GB/s，三代理挤在同一窄区间；本机 nginx/OpenResty 64K 达 3.0 GB/s、1M 达 4.5 GB/s，**lite 仅为其 55% / 60%**，POST 为 86%（对照组改回请求体缓冲后为 91–92%）。均值延迟 64K 19 ms vs 10–12 ms。这是本次最重要的新发现，第 4 节剖析。

### 3.3 HTTP/3（http3_benchmark_client 16 连接×4 流，20s+3s 预热，6 样本中位；OpenResty 无 H3）

| 场景 | 代理 | RPS 中位 | 快档 | Δ vs #3 | p50 ms | p90 ms | p99 ms | 客户端丢包 | 内核 UdpRcvbufErrors/次 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | lite-nginx | 79,518 | 73–82k（5/6）+108k | -10% | 0.77 | 1.02 | 1.53 | 0 | 1–23 |
| GET 1 KiB | **nginx(quic)** | **103,991** | 97–106k（4/6），134k / 53k | +103% | 0.46 | 1.10 | 1.78 | 0 | 8–235 |
| GET 64 KiB | **lite-nginx** | **38,010** | 37.5–38.7k（5/6） | +407% | 1.32 | 2.54 | 4.50 | 0 | 6.5k–13k |
| GET 64 KiB | nginx(quic) | 35,266 | 35–36k（4/6），慢档 25k | +290% | 1.30 | 2.93 | 4.75 | 0 | 2.4k–7k |
| GET 1 MiB | lite-nginx | 2,853 | 2.3–3.2k 连续 | +360% | 18.94 | 46.59 | 103.42 | 0 | 7k–12k |
| GET 1 MiB | **nginx(quic)** | **3,378** | 3.34k（4/6）/ 3.83k | +448% | 10.11 | 33.92 | 92.42 | 0 | 13k–22k |
| POST echo 1 MiB | **lite-nginx** | **1,571** | 1.40k（3）/ 1.72–1.76k（3） | +504% | 13.28 | 132.10 | 318.46 | 0 | 27k–32k |
| POST echo 1 MiB | nginx(quic) | 1,389 | 1.04k（2）/ 1.37–1.57k（4） | +405% | 19.46 | 110.08 | 329.73 | 0 | **106k–136k** |

6 样本逐档列举（升序）：

| 格 | lite-nginx 样本 | nginx(quic) 样本 |
|---|---|---|
| GET 1 KiB | 73.1k, 78.5k, 79.4k, 79.6k, 81.6k, 108.5k | 53.0k, 96.7k, 103.0k, 105.0k, 105.6k, 133.7k |
| GET 64 KiB | 30.3k, 37.5k, 37.8k, 38.3k, 38.5k, 38.7k | 24.9k, 25.1k, 35.2k, 35.3k, 35.6k, 36.2k |
| GET 1 MiB | 2342, 2658, 2670, 3036, 3102, 3173 | 3340, 3341, 3342, 3413, 3825, 3844 |
| POST echo | 1397, 1406, 1422, 1721, 1736, 1764 | 1037, 1040, 1367, 1411, 1421, 1568 |

**要点**

- 本机 H3 绝对吞吐是 #3 的 4–6×（大体）——旧主机 H3 受 `wmem_max` 与 CPU 双重限制；同时多档分布依旧存在（两代理在 s3 同时跳到高档：lite 108k / nginx 134k，指向主机侧状态而非代理），继续以中位数 + 逐档呈现。
- **GET 1 KiB 反转**：nginx-quic 中位 104k（快档 97–106k）vs lite 79.5k（73–82k），nginx +31%，p50 0.46 vs 0.77 ms。lite 的一致性优势仍在（5/6 样本 ±5%），但吞吐不再领先。
- **64K / POST echo** lite 领先 8% / 13%，且 POST 无 nginx 那样的 1.04k 坏档；**1 MiB GET** nginx 领先 18%。
- **UDP 丢包**：客户端 `dropped_datagrams` 全程 0，但内核 `UdpRcvbufErrors`（全机计数，无法区分代理/客户端套接字）在大体场景每次数千到十几万；nginx POST 每次 10.6–13.6 万，为 lite（2.7–3.2 万）的 4×。根因是本机 `rmem_max=208 KB`（#3 主机 33 MB），QUIC 靠重传兜底，0 失败但影响大体绝对值与档位分布。

---

## 4. 差距剖析（矩阵之外单独复现，同绑核；绝对值低于矩阵，比值有效）

方法：15 s 定长压测，采集代理每 worker 的 user/sys CPU（`/proc/<pid>/task/*/stat`）、`LD_PRELOAD` 计数 libc I/O 入口（`temp/bench/libsyscount.so`，`epoll_pwait2` 经 `syscall()` 单独计入）、`perf record`（用户态，`perf_event_paranoid=2`）、`perf stat` 软件事件；脚本 `temp/bench/h1_profile.sh`。文中的源码实验均在临时构建目录验证后回退，未入库。

### 4.1 HTTP/1 GET 1 KiB / 64 KiB（lite vs nginx）

| GET 1 KiB，每请求 | lite-nginx | nginx(quic) | 比 |
|---|---:|---:|---:|
| 吞吐 | 132.5k rps | 205.9k rps | 0.64× |
| 4 个 worker 忙时（15 s） | 14.9 / 14.8 / 14.8 / 14.8 s（全部打满） | 15.1 s ×4（全部打满） | — |
| **CPU / 请求** | **29.8 µs**（user 9.1 + sys 20.7） | **19.4 µs**（user 4.8 + sys 14.5） | **1.54×** |
| 系统调用 / 请求 | 9：`recv` 3、`send` 2、**`epoll_ctl` 4** | 5：`recv` 2、`writev` 2、`getsockopt` 1 | 1.8× |
| `epoll` 每次唤醒事件数 | ~56 | ~50 | 持平 |
| 缺页 / 请求（`perf stat` minor-faults） | 0.165 | 0.0003 | — |
| `pthread_mutex_lock` / futex / setsockopt / madvise | 0 | 0 | 无锁竞争 |

| GET 64 KiB，每请求 | lite-nginx | nginx(quic) |
|---|---:|---:|
| 吞吐 | 75.8k rps | 91.3k rps |
| CPU / 请求 | 52.2 µs（user 13.3 + sys 38.9） | 43.7 µs（user 8.0 + sys 35.6） |
| 系统调用 / 请求 | `recv` 4（**16.0 KiB/次**）、`send` 2（32 KiB/次）、`epoll_ctl` 4 | `recv` 3（21.4 KiB/次）、`writev` 3 |

两边 4 个 worker 都 100% 忙、唤醒批量度一致、无锁竞争，差距完全来自**每请求的内核与用户态路径都更长**（1 KiB：1.54× ↔ 吞吐低 34%）。

**内核态多 6.2 µs（1 KiB）——主因是 epoll 兴趣管理方式**

- **每请求 4 次 `epoll_ctl`，nginx 为 0。** `Efd` 使用 `EPOLLONESHOT`：每次等待 `ADD`/`MOD`，事件触发、回调清空后 `RWFd::sync_interest()` → `Efd::watch_set(None)` → **`EPOLL_CTL_DEL`**（`src/net/detail/Efd.cpp` `watch_set`）；客户端连接等一次、上游连接等一次，每请求两对 ADD/DEL。nginx 用 `EPOLLET` 注册一次后不再调用 `epoll_ctl`，就绪状态由 `rev->ready` 在用户态维护。
- 热缓存微基准（`temp/bench/sysbench.c`）里 ADD+DEL 一对仅 0.56 µs、`MOD` 0.12 µs，但 256 连接真实负载下 socket 结构冷缓存，实测约 **1 µs/次**：实验把 `watch_set(None)` 改为保留注册（ONESHOT 触发后已自动解除武装，仅在仍武装时 `MOD` 清零），`epoll_ctl` 4→2 次/请求，**吞吐 +8～11%**（两轮：129.5k→139.5k、123.5k→137.0k），sys −1.8～2.4 µs/请求。推到 nginx 式持久 `EPOLLET` 注册（0 次）预计再省 ~2 µs，可弥合内核态差距的 ~70%。
- **多 1 次空 `recv`（EAGAIN）**：`StreamFd::read` 是"先 `try_read` 再等事件"；keep-alive 连接发完响应立即读下一个请求，此时客户端尚未发出，必然 EAGAIN（`bytes/recv` 0.4 KiB vs nginx 0.6 KiB 即此）。nginx 在 ET 语义下上次短读后直接等事件。
- 0.165 次缺页/请求来自 glibc malloc 内部的 brk/mmap 伸缩（`__brk/__mmap` 不经 PLT，LD_PRELOAD 计不到）；本机一次 minor fault ≈1.6 µs（`temp/bench/faultbench.c`），折合 ~0.3 µs/请求，次要。

**用户态多 4.3 µs（1.9×）——开销分散在协程分层与分配上**（`perf` 用户态样本，lite 无单项 >6.2% 的热点）

| 类别 | 占 lite 用户态 | ≈ µs/请求 | 说明 |
|---|---:|---:|---|
| `malloc/free/operator new/delete` | ~10% | 0.95 | 每层协程帧堆分配 + 每请求对象；nginx 用 pool，malloc 仅占 1.5% |
| 协程 `.resume` 链 | ~20% | 1.9 | 一个请求穿过 15～20 个嵌套协程帧（`Http1Connection::run → parse_request → RequestDispatcher::handle/handle_inner → run_proxy_request → proxy_over_connection → send_header 4 层薄包装 → pipe_http_body → ClientHttp1Exchange::read_header/read_body → TcpTransport/StreamFd::read/write`），每层帧分配 + 挂起/恢复簿记 |
| HTTP 解析（`RequestLineParser`/`ResponseLineParser`/`header_line`/`HeaderMap`） | ~7% | 0.7 | 比 nginx 便宜（nginx 解析占其用户态 21% ≈ 1.1 µs），不是问题 |
| 定时器堆 `BinaryHeap<TimerEntry>::insert/remove` | ~3% | 0.3 | 每次 I/O 操作装/卸超时；nginx rbtree 定时器占 5%，持平 |
| 连接池 `Lease::reset`/`AcquireAwaiter`、`IoBufChain`、`BufPool` | ~5% | 0.5 | |
| syscall 桩（`recv/send/epoll_ctl` 用户侧） | ~10% | — | 反映系统调用次数多 |

**64 KiB**：结构相同，内核态差距缩到 3.3 µs（64 KiB 拷贝两边一样），但 lite 上游读是 16 KiB 一片（`recv` 4 次，nginx 3 次 × 21 KiB，`proxy_buffer_size 64k`），`pipe_http_body` 每片一轮读/写协程往返，用户态多 5.3 µs。

**内核态采样补充（H1 GET 1 KiB，每响应 µs，`kernel_profile.sh`）**：lite 30.3 vs nginx 20.6，其中用户态 +3.6、唤醒 wrk 的关中断段 +2.8（客户端 CPU 每响应 CAL IPI 0.39 vs 0.27）、syscall 进出 +0.9、`epoll_ctl`（ep_insert/ep_remove + kmem/rcu）+1.5、其余 +0.9。两边最大的单项内核开销都是"唤醒对端"（各占 ~26%）——loopback ping-pong 在 VM 里的固有税，与代理无关。

**优化方向（按预期收益）**：① Poller/Efd 改为持久 `EPOLLET` 注册、就绪状态在 `RWFd` 用户态维护（最小改动即上述实验：不 `DEL`，保留 ONESHOT 注册，已验证 +8～11%；全步预计 +15～20%）；② keep-alive 发完响应后先等事件再 `recv`（或记录上次短读状态）；③ 协程帧 / 每请求对象走 per-request arena（promise 自定义 `operator new` 挂到 `BufPool`），合并 `send_header` 等薄包装层，同时消掉 malloc 引起的缺页；④ 上游读缓冲 16 KiB → 64 KiB。

### 4.2 HTTP/2 GET 64 KiB（lite vs nginx）

同一批次（h2load -t6 -c32 -m16，15 s；脚本 `temp/bench/h2_profile.sh`，`libsyscount.so` 额外用 `CLOCK_THREAD_CPUTIME_ID` 包住每个 libc I/O 调用，得到"每响应各 syscall 内部消耗的 CPU"）：

| 每 64 KiB 响应 | lite-nginx | nginx(quic) | 比 |
|---|---:|---:|---:|
| 吞吐 | 28.1k rps | 43.7k rps | 0.64× |
| 4 个 worker 忙时 | 14.9 s ×4（打满） | 15.0 s ×4（打满） | — |
| **CPU / 响应** | **141.8 µs**（user 44.4 + sys 97.4） | **91.0 µs**（user 36.0 + sys 55.0） | 1.56×（user 1.2×，**sys 1.8×**） |
| 系统调用 / 响应 | `recv` 4.1、`send` 10.1、`epoll_ctl` 2.1 | `recv` 2.0 + `read` 0.9、`write` 7 + `writev` 1、`getsockopt` 1 | 1.4× |
| `send` 尺寸分布 | **<64 B ×4.0**、<1 K ×1.1、<8 K ×1.0、<16 K ×1.0、≥16 K ×3.0 | <64 B ×1、<1 K ×3、≥16 K ×4 | |
| `recv` 尺寸分布 | ≥16 K ×1、<16 K ×1、<64 B ×0.9、**EAGAIN ×1.1** | ≥16 K ×1、<1 K ×1.1、<64 B ×0.7 | |
| **`send`/`write` 内部 CPU** | **63.2 µs** | 41.5 µs | +21.7 |
| **`recv`/`read` 内部 CPU** | **28.3 µs** | 11.2 µs | +17.1 |
| `epoll_ctl` + `epoll_pwait2` 内部 CPU | 2.6 µs | 0.4 µs | +2.2 |
| 缺页 / 响应 | **0.9–1.4** | 0.001–0.02 | |

用户态只差 1.2×（TLS 加密两边都在用户态：lite 热点 `aes_gcm_enc_update_vaes_avx2` 22.9%、`malloc`+`cfree` 5.4%、`TlsTransport::poll_writev` 3.3%、`pipe_http_body` 3.0%、定时器堆 3.4%）；**87% 的差距在内核态，且集中在 `send`（+22 µs）与 `recv`（+17 µs）内部**。逐项实验（每项均与同批次基线交替运行，主机在长时间满载后会降频，只看同批比值）：

| 实验 | 变化 | 结论 |
|---|---|---|
| **glibc 不归还内存**（`GLIBC_TUNABLES=glibc.malloc.trim_threshold=4G:mmap_threshold=4G:top_pad=64M`） | 缺页 1.4→0；**`recv` 内部 28–31→19 µs**；sys −11～21 µs；吞吐 **+4～19%**（30.3k→36.0k、26.6k→31.1k/28.8k） | 主因之一。IoBuf 节点/协程帧等经 glibc 释放时触发 `brk` 收缩，WSL2 把空闲页上报给 Hyper-V，下次 `copy_to_user` 时重新缺页，本机一次缺页折合 10 µs 量级（裸缺页微基准仅 1.6 µs） |
| 帧头+载荷合并成一条 16 KiB TLS 记录（预算与帧上限都取 16375，`kTlsCoalesceMax` 16384） | `send` 10.1→5.6 次（<64 B 记录归零）；sys 不变；user +8 µs（多 4 次 16 KiB memcpy 进 scratch） | 4 条 9 字节帧头小记录在 Nagle 下会被内核合并，**不是**内核开销来源；单独做无收益 |
| 强制 `TCP_NODELAY`（LD_PRELOAD 在 accept4/connect 后设置） | 吞吐/CPU 无变化 | lite **从不设置 `TCP_NODELAY`**（`apply_tcp_socket_options` 只有 nacos/cat 在用；nginx 默认开），对本场景 CPU 无影响，但对交互式小帧是延迟缺陷，应修 |
| `steal off` | 无变化 | 非跨线程竞争 |
| 单 worker | 每响应 sys 更高（112–128 µs） | 开销是 lite I/O 模式固有的，非并行副作用 |
| h2load `-m1` | lite 34.2k vs nginx 42.6k，lite sys 仍 +26 µs | 与多路复用/交错无关 |
| 客户端 CPU 的 RES IPI / h2load 上下文切换 | 0.003 次/响应 | 不是唤醒成本 |
| nginx 关 `tcp_nodelay` 对照 | nginx `write` 内部 34→13.8 µs，代理只用 2.88 核 | loopback 上发送侧内核成本（含对端接收处理与 ACK）会随 Nagle 时序在收发双方之间转移；nginx 的 41 µs 已是"同步完成对端接收"的价格，lite 的 58–69 µs 在有无 NODELAY 下不变 |

**内核态采样（`sudo sysctl kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0` 后，`temp/bench/kernel_profile.sh`：`perf record -e cpu-clock -F 1999 -g`，WSL2 无 PMU 故用 hrtimer 采样；`kgroup.py` 按符号分组，按 3.97 核饱和折成每响应 µs）**

| H2 GET 64 KiB，每响应 µs | lite | nginx | 差 |
|---|---:|---:|---:|
| **唤醒对端**（`tcp_v4_rcv→tcp_data_queue→sock_def_readable→__wake_up_common_lock`，在自己的 `send()` 上下文里同步执行对端接收并唤醒 h2load 线程；关中断段，含 Hyper-V IPI hypercall） | **26.9** | 7.5 | **+19.4** |
| **缺页**（`do_user_addr_fault→__alloc_pages→clear_page_erms`） | **12.0** | 3.0 | **+9.0** |
| 其他内核（memcg 记账、skb 释放、rcu、锁） | 19.8 | 11.5 | +8.3 |
| 用户态 | 40.0 | 33.7 | +6.2 |
| `copyout/copyin` | 10.1 | 7.0 | +3.1 |
| syscall 进出 + fd 查找 | 8.0 | 5.0 | +3.1 |
| tcp 接收路径（softirq） | 7.8 | 5.7 | +2.1 |
| netfilter/conntrack（本机 docker 规则对 loopback 的税，两边都交） | 7.1 | 5.4 | +1.7 |
| `epoll_ctl`（ep_insert/ep_remove） | 1.4 | 0.6 | +0.7 |
| tcp 发送路径 | 4.1 | 3.6 | +0.5 |
| 合计 | 137.3 | 84.0 | +53.2 |

`/proc/interrupts` 佐证：压测中**客户端 CPU 10-15 每响应收到 1.76–1.82 个 CAL（唤醒）IPI（lite）vs 0.25–0.30（nginx）**，`ipi_run.sh`，两轮一致。机制：lite 把一个 64 KiB 响应拆成时间上分离的多个小突发送出（上游 48 K + 16 K 分两次到达就分两次发；`Http2Connection` 出站按流轮询、每轮每流只出 1 帧再 `pump_outbound`），突发之间 h2load 线程已处理完回到 `epoll_wait`，下一个突发到达就要 `try_to_wake_up` → wakelist IPI；在 Hyper-V 里发 IPI 是一次 hypercall（VM exit），同步计在发送方 `send()` 内。nginx 把 4 条记录连续 `write` 出去，对端醒一次全收完。这解释了 §4.2 前面"消掉小记录/关 Nagle 都不省 sys"——决定唤醒次数的是突发的**时间分布**而非 `send` 次数。此项在裸机上（IPI ≈1–2 µs）会小得多，是 VM 放大的开销，但方向不变。

**结论与修法**（lite 多出的 53 µs 中约 45 µs 已归因）：① **输出合并**——每个连接每轮事件循环把所有就绪流的帧攒成一批一次发出（放宽 `payload_budget`/每流每轮帧数，或上游读满 64 KiB 再编码），目标是把每响应的客户端唤醒次数从 1.8 压到 ~0.3（−15～19 µs）；② **内存不归还内核**——内核栈已确认具体链路：`IoBufChain::consume_and_compact/release_nodes → free() → glibc heap_trim → madvise(MADV_DONTNEED) → hyperv_flush_tlb_multi`，随后下一次上游 `recv()` 在 `copyout` 里缺页。根源是上游响应体读缓冲 `IoBuf::allocate(kMaxDirectBodyRead=64 KiB)` 每次读都新分配（上一块已被零拷贝切片给客户端出站链路共享而不 unique），几微秒后随出站完成释放；lite 的 worker 是**线程**，走 glibc 非主 arena，top 空闲 ≥128 KiB 即 `heap_trim`。nginx 同样 malloc/free（每请求 pool + 64 KiB 上游缓冲），但 worker 是**单线程进程**走主 arena（brk），且缓冲随请求存活（512 并发 × 64 KiB 交错驻留），稳态下 top 从不达到 trim 阈值——内核样本里 nginx 的 brk/munmap/madvise 为 0.00%。修法：64 KiB 读缓冲按 loop 做 free-list 复用，或启动时 `mallopt(M_TRIM_THRESHOLD/M_TOP_PAD)`（`GLIBC_TUNABLES` 实验 +4～19%），或 `FIBER_USE_JEMALLOC`（decay 定时批量 purge）；WSL2 空闲页上报使一次缺页折合 ~10 µs，裸机约 1–2 µs + TLB shootdown IPI；③ `recv` 少两次（上游一次收满、先等事件再读）+ `epoll_ctl` 持久注册（§4.1，−3～5 µs）；④ 用户态：每 16 KiB 一轮的 `pipe_http_body` 协程往返与 `poll_writev`（−5 µs 左右）。

对照 **H2 GET 1 KiB**（lite 175.5k 领先 15%）：小响应不经 DATA 分片，无按字节的内核成本，lite 的 HPACK/帧路径更省，所以差距只在大体上出现。

### 4.3 HTTP/3 GET 1 KiB（lite vs nginx-quic）：为什么中位数被反超

**1. 结果首先取决于一次"抽奖"。** `http3_benchmark_client --threads 4` 只有 4 个 UDP 源端口，16 条连接经 `SO_REUSEPORT` 四元组哈希落到 4 个 worker socket 上（lite 与 nginx 都是每 worker 一个 reuseport socket）：4 个哈希值命中的 worker 数 ∈ {1,2,3,4}，概率约 2%/33%/56%/9%，且随每次运行的临时端口变化。矩阵里两代理的"多档"正是命中 worker 数：nginx 53k / 97–106k / 134k ≈ 2 / 3 / 4 个 worker（每 worker ≈33k），lite 73–82k / 108k ≈ 2 / 3 个 worker（每 worker ≈39k）。**lite 6 次里 5 次只命中 2 个 worker，nginx 4 次命中 3–4 个**，这是中位数 79.5k vs 104k 的主要来源。重复 6 次直接观测每 worker 忙时验证了这一点（`temp/bench/logs/dist.json` 系列）：

| 命中 worker 数 | lite（pacing 默认开） | lite（pacing 关） | nginx |
|---|---|---|---|
| 2 | 78.8k | 69.9k | 47.8k |
| 3（全部饱和） | 93.7k、116k | 108.8k、117.5k、118.9k | 92.5k、92.7k、107k |
| 4 | — | 99.1k（一 worker 仅 27% 忙） | 120.8k、123.8k |
| 3 但有 worker 未饱和 | 54.7k、68.3k、104k（某 worker 忙 21–76%） | 83.6k、88.3k、88.3k（某 worker 忙 75%） | 未出现 |

分发均匀时（客户端改为 16 线程 / 16 连接，4 个 worker 全部 100% 忙）两者**持平**：lite 110.3k / 98.5k vs nginx 107.2k / 106.1k，每响应 CPU 36–40 vs 37 µs。lite 的包效率更高：服务端→客户端 0.87 个数据报/响应（nginx 1.72，其中约 1 个是纯 ACK 包），客户端→服务端 0.70 vs 1.17。

**2. lite 的 worker 在轻载时达不到饱和。** 当一个 worker 只分到 1 个客户端线程的 4 条连接（16 个闭环并发）时，nginx 的 worker 仍 100% 忙，lite 的只有 21–75%——闭环吞吐 = 并发数 / 单请求往返，lite 的往返更长。单连接闭环 p50（无排队）：

| 单连接 p50 往返 | lite | lite 关 pacing | nginx |
|---|---:|---:|---:|
| 1 流 | 126 µs（6.2k rps） | 95 µs（8.9k） | 90 µs（9.8k） |
| 4 流 | 149 µs | 150 µs | 135 µs |
| 16 流 | 363 µs（40.5k） | 358 µs | 305 µs（50.1k） |

- **服务端 pacing 默认开启**（`QuicPacingOptions{enabled=true, rate=1.25×cwnd/max(RTT,1ms), max_burst_packets=10, timer_granularity=100µs}`，`include/fiber/quic/QuicPacer.h`）：小响应连接 app-limited、服务端 cwnd 停在初始窗附近，预算又被 10 包封顶，loopback 上 RTT 被钳到 1 ms，于是每次发送都可能撞上 100 µs 粒度的 pacing 定时器——1 流时每请求多 ~30 µs；关掉后 1 流与 nginx 持平，3 worker 满载吞吐从 94–116k 升到 108–119k，"忙 1.5 s"的近空闲 worker 消失。nginx 的 QUIC 不做 pacing。
- 关掉 pacing 后 4/16 流仍慢 10–19%（149 vs 135、358 vs 305 µs）：这是每请求路径本身的固定开销（含 §4.1 的上游 H1 跳：EAGAIN 探测读 + `epoll_ctl`；以及 H3 帧/QUIC 打包每轮的协程与定时器操作），在低并发下直接变成往返延迟、变成轻载 worker 的欠饱和。

**结论**：H3 GET 1K 的"反超"不是 lite 的 QUIC 栈每响应更贵（同等分发下持平，per-worker 甚至更高），而是 (a) 4 源端口客户端下的 reuseport 分发抽奖，(b) lite 轻载 worker 因默认 pacing 与更高的低并发往返延迟而欠饱和。**建议**：① H3 小请求压测改用 ≥16 个客户端源端口（`--threads 16`）或按"命中 worker 数"分层报告，否则单次数值无意义；② pacing 在 RTT 低于定时器粒度时应旁路（或按 RTT 自适应粒度、放宽 `max_burst_packets`），至少 loopback/内网场景默认关闭；③ 降低每请求固定延迟（§4.1 的 epoll/EAGAIN 项对 H3 同样生效）。

## 5. 与 #3 的逐项对照结论

1. **保持的优势**：H1 POST echo（流式对照 1.22×，默认对照 1.8×）；H2 GET 1K 领先 13–15%（更稳）；H3 64K / POST echo 领先 8% / 13%，且无坏档。
2. **修正的结论**：（a）#2/#3 "H1 POST 2.6×" 约一半来自对照组请求体缓冲落盘，流式对流式为 1.2×；（b）#2/#3 "H2 大体 harness-bound 三者持平" 在本机不成立——lite H2 64K/1M 仅为 nginx 的 55–60%，每响应 CPU 1.5–1.9×（§4.2：唤醒对端 IPI +19 µs、内存归还缺页 +9 µs、recv/epoll_ctl/用户态各数 µs，均已归因）；（c）H3 GET 1K 中位数由 lite 领先变为 nginx-quic 领先 31%，但 §4.3 证明这是 4 源端口 reuseport 分发抽奖 + lite 默认 pacing 在轻载 worker 上的欠饱和；分发均匀时持平，#3 的 H3 多档分布也应按"命中 worker 数"重新解读。
3. **持续的劣势**：H1 GET 1K 落后 34%、GET 64K 落后 29%；H1 GET 1K 偶发 >50 ms 尖刺（#3 已记录）。
4. **新主机效应**：H3 绝对吞吐 4–6×、H2 大体 3–7×；H1 GET 1M 上限低 ~15%；`rmem_max` 小导致 H3 大体内核丢包。

## 6. 局限

1. **跨主机**：与 #3 的绝对值差异主要来自硬件/内核/工具版本（nginx 1.31.3、h2load 1.59、OpenSSL 1.1.1w），只应比较同机相对值。
2. **WSL2**：vCPU 由 Hyper-V 在 6P+8E 混合核上调度，taskset 只能减少客体内争用；实时时钟回拨已检测并规避（第 2.1 节），但无法排除 hypervisor 调度带来的档位跳变（H3 s3 两代理同时升档）。 WSL2 无 PMU（`cycles` 不可用）且默认 `perf_event_paranoid=2`；放开 sysctl 后可用 `cpu-clock` 采内核样本（第 4 节即此），kprobes/BTF/ftrace 也齐全（`bpf_tcp.bt`、`ftrace_sendmsg.sh` 备用）。**IPI/缺页在 VM 里的代价（hypercall、空闲页上报）比裸机高一个量级**，§4.2 的绝对差值会因平台而异，方向不变。
3. `rmem_max=wmem_max=208 KB` 且无 sudo：H3 大体场景存在内核侧 UDP 接收丢包，绝对值偏低；并发维持 16×4。**H3 客户端只有 4 个源端口，服务端 worker 命中数随机（§4.3），H3 小请求的单次数值与"档位"主要反映该抽奖，后续应改为 ≥16 线程或按命中 worker 数分层。**
4. H2 仅 2 样本（OpenResty GET 1K 离散 ±12%），H1 3 样本；第 4 节剖析为单次 15 s 复现，只取比值。
5. 对照组 `proxy_request_buffering off` 与 #2/#3 不同（更公平但不同口径），已用附加实验同时给出两种口径。

## 7. 复现

```bash
# 构建（Release + ThinLTO + 静态 libc++ + clang-22）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-22 \
  -DCMAKE_CXX_COMPILER=clang++-22 -DFIBER_USE_LIBCXX=ON -DFIBER_BUILD_TESTS=OFF
cmake --build build --target http_benchmark_backend fiber_app_lite_nginx http3_benchmark_client

# 工具链与配置均在 temp/bench/（本次重建；不入库）：bin/{wrk,h2load}、openresty-install/、
# lite_nginx.conf / openresty.conf / nginx_quic.conf（+ *_reqbuf.conf）、cert.pem、post_1m.bin、post_1m.lua
bash temp/bench/backend.sh start                  # taskset -c 0-3 http_benchmark_backend 19001

# 多样本矩阵（需 >10 min，建议 setsid nohup 脱离终端）；结果 temp/bench/results2/s{N}/
bash temp/bench/run_matrix.sh h1 3 && bash temp/bench/run_matrix.sh h2 2 && bash temp/bench/run_matrix.sh h3 6
bash temp/bench/run_reqbuf.sh                     # 附加实验 → temp/bench/results_reqbuf/

# 汇总（中位数 + Δ vs #3 内嵌基线 + 时钟回拨样本标注）
python3 temp/bench/parse_results2.py              # → 亦保存为 temp/bench/results2/summary.md
```

剖析脚本：`temp/bench/{h1,h2,h3}_profile.sh`（每 worker CPU + `libsyscount.so` 系统调用计数/内部耗时 + `perf stat`）、`kernel_profile.sh` + `kgroup.py`（内核采样与分组，需 `sudo sysctl -w kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0`）、`ipi_run.sh`/`ipi_run_h1.sh`（每响应 IPI）、`bpf_tcp.bt`（bpftrace，需 root）、`ftrace_sendmsg.sh`（function_graph，需 root）。

原始数据：`temp/bench/results2/s1..s6/`（H3 含 `.json` 与 `nstat` 前后快照）、`temp/bench/results2_s1_clockstep_h1/`（时钟回拨影响的原始 H1 s1）、`temp/bench/results_reqbuf/`、`temp/bench/logs/`（`matrix.log`、`clockwatch.log`、`hostclock.log`、`lite_h2_64k.perf.data`、`sc_*` 系统调用计数）、`temp/bench/env.txt`。
