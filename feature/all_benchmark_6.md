# lite-nginx 反向代理全协议压测复测报告（#6）

执行日期：2026-09-26
被测对象：`apps/lite_nginx` @ git `3e71e01a`（`tls` 分支；Release + LTO + libc++，clang-20，构建配置逐项核对与 #3/#5 一致）
对比对象：OpenResty 1.25.3.2、nginx 1.31.1 (quic)（与 #2/#3/#5 完全相同的构建与配置）
基线：[all_benchmark_5.md](all_benchmark_5.md)（2026-09-14，git `a0263f3`，同机）
方法论：与 #3/#5 完全一致（场景/参数/绑核/公平性配置），多样本取中位数：H1×3、H2×2、H3×6。

> **本次测量的是 `tls` 分支**：距 #5 共 40 提交，核心是自研 TLS 引擎落地与传输面收窄——`26dda4c8`（net TLS 换芯到自研引擎）、`e44ea906`（fiber_lib 收窄为 boringssl::crypto）、`996383da`/`0b24d4b1`（QUIC 握手重写上自研引擎）、`7de1a596`（HttpTransport I/O 收窄为追加式 readv + 单次 writev）、`3e71e01a`（record framing 移入 glue）。即：**#5 报告中 `a58cf18`（跨节点填满 TLS record + 每操作批量化 DATA 帧）所在的发送路径已被整体重构**，本次复测即检验其性能是否存续。

---

## 1. 结论速览（vs 基线 #5）

| 维度 | 本次结论 | vs #5 |
|---|---|---|
| **H2 大体（64K/1M/POST）** | lite **回吐全部优势**：64K 10.4k（-53%）、1M 733（-59%）、POST 356（-58%），三格几乎精确回到 #3 水平（#3：9.6k/712/358）；对 nginx 跌破持平线（0.89×/0.89×/0.94×），对 OpenResty 仅余 1.19-1.30× | #5 的 2.0-3.0× 全面反超**作废** |
| **H2 GET 1 KiB** | lite 159.8k（-11%），仍领先：OR 的 1.24×、nginx 的 1.39×（#5 为 1.47×/1.64×） | 领先收窄但保持 |
| **H3 大体（每 worker 归一）** | lite 64K ~2.7k/w（**-58%**）、1M ~190/w（**-62%**）、POST ~125-142/w（约 -45~-50%）；**64K/1M 每 worker 跌破 nginx（0.90×），POST 余 ~1.4×** | #5 的 2.2-2.7× **作废**，两格反转为劣势 |
| **H3 GET 1 KiB** | lite 每worker ~32k vs nginx ~27k，**1.19×**（#5 1.44×）；绝对中位 95.4k vs 82.8k | 领先收窄但保持 |
| **POST echo 1 MiB（H1）** | lite 5,873 rps / 5.74 GB/s，为对照的 **2.72×** | #5 2.55×，略增 |
| **HTTP/1 GET 1 KiB** | lite 144.5k，落后对照（218.9-225.1k）**35-36%** | #5 落后 38%，小幅收窄 |
| **GET 1 MiB（H1）** | 三者 9.9-10.2k（~10 GB/s）打平 | 不变，带宽封顶 |
| **稳定性** | 全部有效测量 0 失败 / 0 非 2xx / H3 全程 0 UDP 丢包；wrk 时长全部 20.03-20.10s | 不变 |

一句话：**自研 TLS 引擎换芯 + 传输面重构后，`a58cf18` 带来的 H2/H3 大体优势被完整回吐——H2 大体三格回到 #3 水平、H3 大体每 worker 跌破 nginx；H1 面不受影响（POST 2.72× 与 GET 1K 落后 ~36% 的旧格局不变），H2/H3 小请求仍领先但收窄。这确认了 09-slice3 已记录的 proxy bulk -54~-60% 回归，并将其定位扩展到 H3（指向 H2/H3 共享的发送路径，而非仅 TLS 记录层）。**

> **勘误（2026-09-26 排查完成，见 §7）**：上段"指向共享发送路径"的推断**已被证伪**。真实根因有二，均与发送路径重构无关：
> 1. **密码套件偏好变化**（代码回归，**已修**，见 §7.1/§7.5）：自研引擎固定服务端偏好 AES 优先（`kServerSuites`），丢失 BoringSSL"无 AES 硬件时 ChaCha20 优先"的运行时行为。本机 CPU 无 AES-NI，AES-GCM(vpaes) 228 MB/s/core vs ChaCha20-Poly1305 ~600+ MB/s/core，H2/H3 大体全被压低 ~2.4×。
> 2. **`temp/_deps` 构建污染**（环境事故，已修复）：`boringssl-build` 被共享 `FETCHCONTENT_BASE_DIR` 上的一次 Debug 配置复写为 -O0，poly1305 C 回退慢 2-3×；`#5` 的 h1/h2 原始数据亦被 09-23 的污染期重跑覆盖。
> 修复 libcrypto 后强制 ChaCha：1M **1816**（#5 报告 1771 ✓ 复现）、64K **24.7k**（#5 22.1k ✓）、1K **202.6k**（#5 178.6k，±13%）。

---

## 2. 环境与构建（与 #5 的差异点）

- 同一台 16 vCPU / 32 GiB / Ubuntu 20.04 主机；sysctl 未变（`rmem_max=33MB`、`wmem_max=208KB`，本次实测一致）。
- 绑核不变：backend 0-3 / proxy 4-9 / loadgen 10-15。
- 后端、wrk 4.2.0、h2load nghttp2/1.65.0、OpenResty 1.25.3.2、nginx 1.31.1、证书、POST 体、代理配置全部复用 `temp/bench/`（版本逐一复核）。
- lite-nginx 从 `a0263f3` 换到 `3e71e01a`；CMakeCache 逐项核对：Release / LTO ON / libc++ / clang++-20 / 无 jemalloc，与 #3/#5 一致。
- 后台负载与 #5 同级（idle cat-demo nginx 0% CPU 常驻）。
- 本次脚本：`run_matrix6.sh` / `run_h3_6.sh`（写 `results6/s*/`，轮间等待端口释放）、`parse_results6.py`（内嵌 #5 中位基线）；其余复用 #5。
- 冒烟轮先行验证换芯后互操作：curl TLS1.3+ALPN h2 握手 200、h2load 3s 零失败、H3 客户端零失败零丢包——三协议互操作干净。

---

## 3. 结果（每协议中位数；Δ 相对 #5 数值）

### 3.1 HTTP/1（wrk -t6 -c256 -d20s，3 样本中位；nginx 为 4 样本，见 §5.5）

| 场景 | 代理 | RPS | Δ vs #5 | 离散 | p50 ms | p90 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | lite-nginx | 144,464 | +11% | ±1% | 1.62 | 2.36 | 5.45 |
| GET 1 KiB | OpenResty | **225,147** | +5% | ±1% | 1.04 | 1.40 | 2.47 |
| GET 1 KiB | nginx(quic) | 218,852 | +5% | ±3% | 1.08 | 1.46 | 6.61 |
| GET 64 KiB | lite-nginx | 77,948 | +13% | ±2% | 3.01 | 4.09 | 5.16 |
| GET 64 KiB | OpenResty | **95,188** | +14% | ±0% | 2.48 | 3.37 | 6.77 |
| GET 64 KiB | nginx(quic) | 92,873 | +11% | ±2% | 2.54 | 3.58 | 5.59 |
| GET 1 MiB | lite-nginx | 10,072 | +3% | ±1% | 23.65 | 30.66 | 38.68 |
| GET 1 MiB | OpenResty | **10,239** | +1% | ±2% | 23.39 | 30.81 | 43.75 |
| GET 1 MiB | nginx(quic) | 9,866 | -5% | ±3% | 24.41 | 32.09 | 39.82 |
| POST echo 1 MiB | **lite-nginx** | **5,873** | +9% | ±1% | 37.22 | 65.43 | 82.23 |
| POST echo 1 MiB | OpenResty | 2,151 | +2% | ±0% | 108.75 | 142.62 | 267.22 |
| POST echo 1 MiB | nginx(quic) | 2,160 | +1% | ±1% | 110.99 | 144.45 | 311.69 |

样本明细（lite get1k）：141.9k / 145.4k / 144.5k。全部 0 错误 / 0 非 2xx，时长 20.03-20.10s。

**要点**

- **三代理同向 +1~+14%**：整机较 #5 略快（两对照 get1k 均 +5%），lite 各格变化在对照漂移范围内——**H1 面换芯后无回归**。换芯只动 TLS/QUIC 路径，H1 明文代理不经过，符合预期。
- **GET 1 KiB**：lite 144.5k，落后对照 35-36%（#5 为 38%）；p99 5.45ms 仍显著劣于 OpenResty 2.47ms——跨 #3/#4/#5/#6 的遗留排查项不变。
- **POST echo 1 MiB**：5,873 rps / 5.74 GB/s，对照的 2.72×（#5 2.55×）；对照仍为 #2/#3 原配置（请求体缓冲 on），流式对照下收窄的 #4 结论仍适用。

### 3.2 HTTP/2（h2load -t6 -c32 -m16 -D20s TLS，2 样本中位；nginx 为 3 样本，见 §5.5）

| 场景 | 代理 | RPS | Δ vs #5 | 离散 | BW MB/s | mean ms | max ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **159,840** | -11% | ±2% | 167 | 3.10 | 61.70 |
| GET 1 KiB | OpenResty | 129,228 | +7% | ±0% | 137 | 4.34 | 36.29 |
| GET 1 KiB | nginx(quic) | 114,660 | +5% | ±0% | 121 | 4.55 | 26.97 |
| GET 64 KiB | lite-nginx | 10,366 | -53% | ±1% | 649 | 46.21 | 113.73 |
| GET 64 KiB | OpenResty | 8,721 | +2% | ±1% | 546 | 54.95 | 148.68 |
| GET 64 KiB | **nginx(quic)** | **11,667** | +4% | ±0% | 731 | 43.78 | 102.05 |
| GET 1 MiB | lite-nginx | 733 | -59% | ±0% | 741 | 683.70 | 906.54 |
| GET 1 MiB | OpenResty | 592 | +2% | ±0% | 593 | 805.13 | 5,050.00 |
| GET 1 MiB | **nginx(quic)** | **820** | +2% | ±0% | 821 | 611.92 | 1,280.00 |
| POST echo 1 MiB | lite-nginx | 356 | -58% | ±1% | 357 | 1,385.00 | 1,865.00 |
| POST echo 1 MiB | OpenResty | 274 | +1% | ±0% | 275 | 1,705.00 | 4,220.00 |
| POST echo 1 MiB | **nginx(quic)** | **380** | +2% | ±0% | 381 | 1,240.00 | 3,190.00 |

全部 0 failed / 0 timeout；两对照各格对 #5 变化 ≤7%（环境等价性成立）。

**要点**

- **本次最大的变化：lite 大体三格 -53%~-59%，几乎精确回到 #3 水平**（#3：64K 9,626 / 1M 712 / POST 358）——`a58cf18` 的优势在换芯+重构后被完整回吐。三格对 nginx 跌破持平（0.89×/0.89×/0.94×），对 OpenResty 仅余 1.19-1.30×。mean 延迟同步翻倍（1M 684ms vs #5 287ms；POST 1,385ms vs #5 591ms）。
- **GET 1K 相对健壮**（-11%）：小报文单 record 可装满，跨节点填充缺失的代价小；仍领先两对照（1.24×/1.39×），但幅度从 #5 的 1.47×/1.64× 收窄。
- 与在册事实闭环：09-slice3 已记录 proxy bulk -54~-60% 且 record 层转录假设被微基准证伪（记录层与 bssl 打平、转录仅 +2~4%）；本次同幅度端到端复现，**且 H3 大体同样回吐（见 3.3）——回归跨 H2/H3 而明文 H1 无恙，指向二者共享的发送路径（帧打包/批量化/汇聚），而非仅 TLS 记录层**。真实热点仍待 proxy 级 profile。

### 3.3 HTTP/3（http3_benchmark_client 16 连接×4 流，20s+3s 预热，6 样本中位；OpenResty 无 H3）

| 场景 | 代理 | RPS 中位 | Δ vs #5(中位) | p50 ms | p90 ms | p99 ms | 丢包 |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **95,415** | +2% | 0.56 | 0.97 | 1.45 | 0 |
| GET 1 KiB | nginx(quic) | 82,813 | +35% | 0.59 | 1.23 | 1.79 | 0 |
| GET 64 KiB | lite-nginx | 6,837 | -48% | 9.20 | 13.68 | 18.05 | 0 |
| GET 64 KiB | **nginx(quic)** | **9,153** | +5% | 5.45 | 10.72 | 13.82 | 0 |
| GET 1 MiB | lite-nginx | 475 | -68% | 100.61 | 178.94 | 209.15 | 0 |
| GET 1 MiB | **nginx(quic)** | **630** | +2% | 78.98 | 156.93 | 174.34 | 0 |
| POST echo 1 MiB | lite-nginx | 284 | -62% | 173.31 | 339.46 | 370.69 | 0 |
| POST echo 1 MiB | **nginx(quic)** | **279** | +22% | 175.36 | 351.74 | 387.58 | 0 |

双峰/多档样本逐档列举（档位 = 命中的 worker 数，机制同 #4/#5）：

| 格 | lite-nginx 样本 | nginx(quic) 样本 |
|---|---|---|
| GET 1 KiB | 35.8k, 67.6k, 94.5k, 96.3k, 97.0k, 128.4k（1w/2w/3w/4w） | 52.4k, 52.6k, 82.2k, 83.5k, 83.9k, 85.5k（2w/3w） |
| GET 64 KiB | 5.4k, 5.5k, 5.6k, 8.0k, 8.3k, 10.9k（2w/3w/4w） | 9.05-9.29k ×5, 12.0k（3w/4w） |
| GET 1 MiB | 375, 378, 383, 567, 570, 570（2w/3w） | 423, 625, 629, 631, 633, 846（2w/3w/4w） |
| POST echo | 190, 283, 284, 286, 376（+285）（1w/2w/3w） | 185, 189, 279, 280, 283, 285（2w/3w） |

按档位归一的每 worker 吞吐（档值 ÷ worker 数；各档全部符合等差量化）：

| 格 | lite 每 worker | #5 lite/w | nginx 每 worker | #5 nginx/w | lite/nginx |
|---|---:|---:|---:|---:|---:|
| GET 1 KiB | ~32k | ~36k（-11%） | ~27k | ~25k | **1.19×**（#5 1.44×） |
| GET 64 KiB | ~2.7k | ~6.5k（**-58%**） | ~3.0k | ~2.9k | **0.90×**（#5 2.2×） |
| GET 1 MiB | ~190 | ~505（**-62%**） | ~210 | ~205 | **0.90×**（#5 2.5×） |
| POST echo | ~125-142 | ~250（约 -45~-50%） | ~94 | ~92 | ~1.4×（#5 2.7×） |

**要点**

- **nginx 侧每 worker 与 #5 完全一致**（26-28k / 3.0k / 210 / 94，各档等差干净）——环境等价，回归为 lite 独有。
- **lite 大体每 worker -45%~-62%**：64K/1M 跌破 nginx（0.90×），POST 余 ~1.4×，GET 1K 余 1.19×。大体 p50 同步恶化（1M 100.6ms vs nginx 79.0ms，#5 时 lite 全面占优）。
- H3 数据面不走 `TlsStreamFd`（QUIC 自带包保护，本次仅握手重写上自研引擎；20s 长连接下握手成本已摊薄）——**H3 大体回吐与 H2 同幅，进一步把嫌疑从"TLS 记录层"推向 H2/H3 共享的发送路径**（`a58cf18` 的"每操作批量化帧"在 `7de1a596` 传输面收窄中失效是首要候选）。
- 6 样本 × 2 代理 × 4 场景全程 0 丢包、0 失败。

---

## 4. 与 #3/#5 的逐项对照结论

1. **作废的结论**：#5 "H2 四场景全面反超（2.0-3.0×）、H3 每 worker 2.2-2.7×"——`tls` 分支上被完整回吐：H2 大体三格回到 #3 水平、H3 大体两格（64K/1M）跌破 nginx。
2. **保持的结论**：H1 面旧格局不变（POST echo 2.72×、GET 1K 落后 ~36%、1M 带宽打平）；H2/H3 小请求 lite 仍领先（H2 1K 1.24-1.39×、H3 1K 1.19×/w），幅度收窄。
3. **新增的定位信息**：回归同幅命中 H2 大体与 H3 大体而明文 H1 无恙（对照全程零变化）——~~嫌疑集中于 H2/H3 共享发送路径~~ **已证伪**：根因为套件偏好（AES 优先）+ libcrypto -O0 污染两事叠加，均与发送路径无关，详见 §7。
4. **稳定性结论不变**：换芯后三协议与 wrk/h2load/http3_benchmark_client 互操作全程零失败零丢包。
5. **H2/H3 对照数据与 #5 完全可比**（≤±7%），#6 与 #5 的 lite 差值即换芯+重构的净效应。

## 5. 局限（在 #5 基础上更新）

1. 单机同置、绝对值偏低估——不变（相对对比有效）。
2. H3 档位抽样仍随机（本次 lite 1-4w 档均出现，nginx 未出 2w 以下档）；已用每 worker 归一口径缓解。
3. `wmem_max=208KB` 限制不变，H3 并发维持 16×4 保守值。
4. H2 lite 仅 2 样本、H2/H3 lite 离散极小（±0-2%），回吐幅度远超采样噪声，结论稳健；但更细的分位数对比需更多样本。
5. **端口竞态根因修正（本次 3 轮 nginx 失败+补测）**：本次 h1-s1/s3、h2-s1 三轮 nginx START 失败（`bind() 0.0.0.0:38080 EADDRINUSE`）。根因与 #5 的"停止后端口未释放"解读不同：**38080/38443 落在临时端口范围 32768-60999 内**（lite/OpenResty 的端口均在 32768 以下故从未中招）——压测期间出站连接抽中 38080 作源端口，其 ESTABLISHED/TIME_WAIT 态 socket（非 LISTEN，`ss -tuln` 不可见）即挡住 nginx bind。修复：START 前 bind 预探（本次补测采用，零失败）；长期建议 bench 端口移出临时端口范围。污染样本已剔除存档于 `results6/rejected_nginx_port_race/`，nginx 侧 h1 补测 3 样本（s7-s9）、h2 补测 2 样本（s7/s8），补测值与未污染样本一致。

## 6. 复现

```bash
# 构建（核对 Release/LTO/libc++/clang-20 与 #3/#5 一致）
cmake --build build --target http_benchmark_backend fiber_app_lite_nginx http3_benchmark_client

# 后端
taskset -c 0-3 ./build/example/http_benchmark_backend 19001 &

# 多样本矩阵（run_matrix6/run_h3_6 写 results6/；nginx 轮前有 bind 预探）
bash temp/bench/run_matrix6.sh h1 3   # → temp/bench/results6/s{1,2,3}/
bash temp/bench/run_matrix6.sh h2 2
bash temp/bench/run_h3_6.sh 6

# 修复后验证矩阵（#7，见 §7.6：H3 档位逐样本实测内置）
bash temp/bench/run_matrix7.sh h1 3   # → temp/bench/results7/
bash temp/bench/run_matrix7.sh h2 2
bash temp/bench/run_h3_7.sh 6

# 汇总（中位数 + Δ vs #5/#6 内嵌基线 + H3 每档归一）
python3 temp/bench/parse_results7.py
```

原始数据：`temp/bench/results6/s1..s9/`（H3 含 `.json` 摘要；`s7-s9` 为 nginx 端口竞态补测；`rejected_nginx_port_race/` 为剔除样本）。

---

## 7. 排查后记：H2/H3 大体"回吐"的真实根因（2026-09-26 深挖，修正 §1/§3/§4 的推断）

### 7.1 结论

§1 中"H2/H3 共享发送路径（帧批量打包/跨节点汇聚）失效"的假设**不成立**——`a58cf18` 的跨节点 coalesce 代码在 `3e71e01a` 上完好且未命中 profile。真实根因是两个独立因素的叠加：

| # | 因素 | 性质 | 影响面 | 状态 |
|---|---|---|---|---|
| 1 | **引擎套件偏好固定 AES 优先**：`kServerSuites`/`kClientSuites`（`src/tls/handshake/TlsServerHandshakeShared.h:30`、`TlsClientHandshakeShared.h:31`）丢掉了 BoringSSL "无 AES 硬件时 ChaCha20-Poly1305 优先"的运行时偏好（`ssl_cipher.cc` 的 `EVP_has_aes_hardware()` 检查，客户端/服务端皆然） | **代码回归**（硬件相关：仅在无 AES-NI 主机上吃亏） | H2 大体（TLS 记录密封）+ H3 大体（QUIC 包保护），每字节成本 228→600+ MB/s/core，**~2.4×** | **已修复**（2026-09-26）：新增 `TlsSuitePreference` 模块，客户端 offer 与服务端偏好共用 `EVP_has_aes_hardware()` 硬件感知有效序——无 AES 硬件时 ChaCha20-Poly1305 提到各版本组最前（BoringSSL 语义），AES-NI 机器零变化；验证见 §7.5（快速）/ §7.6（全矩阵） |
| 2 | **`temp/_deps` 共享构建目录被 -O0 复写**：`FETCHCONTENT_BASE_DIR` 全局共享，`build-debug`（Debug，2026-09-26 11:00）及更早（09-14~09-23 间）某次无优化配置把 `boringssl-build` 的 C++ 旗标烤成无 -O → poly1305 **C 回退**（本 CPU 无 SSE4.1，组合汇编 `chacha20_poly1305_seal_sse41/avx2` 不可调度）从 ~600+ 掉到 ~257 MB/s/core。AES 走 vpaes/ghash 汇编不受影响 | **环境事故** | 09-23 重跑（覆盖 #5 h1/h2 原始文件的 704 rps）、#6 中所有 chacha 分支（当时即使强制 ChaCha 也只有 781） | **已修复**：`cmake -S . -B build` 重配置后 -O3/-DNDEBUG 恢复 |

另有一桩**数据完整性事故**放大了混乱：`results5/` 的 h1/h2 文件被 09-23 17:07-17:34 的一次重跑覆盖（mtime 证据），h3 文件仍是 09-14 原始——导致"基线"内部自相矛盾：#5 报告的 H2 数字（1771/22.1k/178.6k，真实）与残留文件（704/10.2k/158.8k，污染期）无法互相印证，误导排查方向。**教训：results 目录应只追加、按日期归档，不可被重跑覆盖。**

### 7.2 证据链

1. **密码学天花板反推**：本机（Core 2 T7700 世系 VM，16 vCPU，无 AES-NI/PCLMULQDQ/SSE4.1）BoringSSL 单核密封速率直接实测：AES-128-GCM（vpaes+ghash 汇编）**228 MiB/s/core**；ChaCha20-Poly1305（chacha 汇编 + **C 版 poly1305**，因无 SSE4.1 组合汇编不可用）污染期 **257** → -O3 修复后大幅上升。4 worker ⇒ AES 绝对上限 ~912 MB/s。#5 报告 1M=1857 MB/s 超限 ⇒ #5 必然协商 ChaCha（BoringSSL 无 AES 硬件时服务端也把 ChaCha 提前，实测 a0263f3+h2load 默认 offer 协商出 `TLS_CHACHA20_POLY1305_SHA256`）；#6 的 741 MB/s ≈ AES 上限的 81%（profile 显示 69% CPU 在密封）⇒ #6 协商 AES。
2. **修复后复现闭环**：`cmake -S . -B build` 重配置（共享 boringssl-build 恢复 -O3）→ 重建 `3e71e01a` → h2load 强制 `--tls13-ciphers TLS_CHACHA20_POLY1305_SHA256`：**1M 1816 / 64K 24,746 / 1K 202,632**——与 #5 报告（1771/22,149/178,596）全面吻合，证实 #5 数字真实且"回吐"非发送路径所致。同二进制默认（AES）：725/10,972/168,650——即 §3.2 实测值，**当前引擎在这台机器上的代价就是套件选择**。
3. **H3 与污染无关**：libcrypto 修复后 H3 1M 仍为 381/572/379（与 #6 持平）——引擎 QUIC 协商 AES（汇编路径不受 -O 影响），H3 回归完全归因于套件偏好（#5 ChaCha ~500/w → #6 AES ~190/w，228 MB/s/core × 0.85 ≈ 190 ✓）。
4. **profile 铁证**（LD_PRELOAD SIGPROF 采样，`timer_create(CLOCK_THREAD_CPUTIME_ID)` 每线程臂装——注意 Linux `setitimer(ITIMER_PROF)` 是 per-thread 的，进程级 CPU 定时器信号只落创建线程，两者都会漏采 worker）：#6 引擎+AES 下 69% CPU 在 `vpaes/gcm_ghash_ssse3`；BoringSSL+ChaCha 下 67% 在 `poly1305_blocks`(C)+`ChaCha20_ctr32_ssse3_4x`。两者速率接近 ⇒ 套件 A/B 仅 ±7-8%（#6 期间）——这曾一度证伪"密码学瓶颈"，实为两个慢实现互为替身；修复后同 A/B 拉开到 2.5×。
5. **对照数据自洽**：nginx-quic（静态链接项目 BoringSSL，未被污染波及）H2 1M 805→820、H3 616→630 稳定；OpenResty（自带 OpenSSL）577→592 稳定；lite H1（无 crypto）5380→5873 稳定——机器本身速度未变。

### 7.3 修复与建议

- **已修复（环境）**：`cmake -S . -B build` 重配置即可把 -O3 烤回共享 `boringssl-build`。**长期**：`cmake/Deps.cmake` 应对 `crypto` 目标强制注入 Release 级优化旗标（不随父构建类型漂移），或强制非 Release 构建使用独立 `FIBER_DEPS_DIR`（`build-debug`/`build-asan` 目前都打在共享 deps 上，是反复污染的根源；[[quic-awaiter-destruction-safe-fix]] 已有前科记录）。
- **待修（代码）→ 已修**：套件偏好硬件感知已落地（`src/tls/handshake/TlsSuitePreference.{h,cpp}`）：`kTlsSuitePreference` 单表取代原先两侧值相同的 `kServerSuites`/`kOfferedSuites`，`tls_effective_suite_order()` 一次性按 `EVP_has_aes_hardware()` 返回注册序或 ChaCha 前置序（每个版本组内稳定前置，1.3 组仍在 1.2 组前），客户端 CH offer 与服务端 `tls_server_suite_select`/`_12` 偏好走查均用之；QUIC 经引擎自动覆盖。新增 `TlsSuitePreferenceTest`（纯置换 + 探针一致性 + 偏好走查三形态），`TlsServerHandshakeEngineTest` 的 1.2 ECDSA 断言改为硬件感知期望。2437 测试全绿。
- **方法论**：results 目录只追加；跨天对比前先跑基准锚（如 cipher A/B + profile）确认环境未漂移。

### 7.4 重跑验证（2026-09-26，libcrypto 修复后的干净环境）

修复 `-O3` 后用同构建、同参数完整重跑 #6 矩阵（新数据 `results6b/`，**不覆盖** `results6/`；脚本 `run_matrix6b.sh`/`run_h3_6b.sh`/`parse_results6b.py` 由 #6 脚本 sed 派生，已快照至 `scripts/benchmark/all/`）：

| 面 | results6b vs #6 中位 |
|---|---|
| H1 全部 12 格 | ±2% 以内（lite POST 5,866 vs 5,873） |
| H2 全部 12 格 | lite 四格 -2~+2%（1M 726 vs 733、64K 10,552 vs 10,366、POST 358 vs 356、1K 156.9k vs 159.8k）；两对照 ±3% |
| H3（每 worker 归一） | 1M ~190/w、64K ~2.7k/w、POST ~95/w——逐档与 #6 一致（绝对中位受档位抽样扰动 ±12~20%） |

全部 0 失败 / 0 非 2xx / 0 丢包。**结论**：#6 的测量本身有效——污染只拖累 chacha-poly 路径，而默认协商（引擎 AES 优先）从不选中它；H2/H3 大体与 #5 的差距即 §7.1 因素 1（套件偏好）的净效应，修复潜力以强制 ChaCha 的 1816/24.7k/202.6k 为上限（§7.2）。

### 7.5 套件偏好修复后的端到端验证（2026-09-26，同机快速验证）

套件偏好修复（§7.1 因素 1）落地后，同构建同参数快速验证（h2load 默认 offer + `-v` 确认协商 `TLS_CHACHA20_POLY1305_SHA256`；-D15s 无预热，H3 20s+3s）。H3 每样本**实测档位**（SIGPROF 采样器的 per-tid 样本分布 + `/proc/<pid>/task/*/stat` jiffies 差值双法，见本节末勘误）：

| 格 | 修复后（默认协商 ChaCha） | vs #5（档对档） | vs #6（AES） |
|---|---:|---|---|
| H2 GET 1M | ~1,840 | ≈1,771 ✓ | 733（+151%） |
| H2 GET 64K | ~24.8k | ≈22.1k ✓（+12%） | 10.4k（+139%） |
| H2 GET 1K | ~177k | ≈178.6k ✓ | 159.8k（+11%） |
| H3 GET 1M | 1,545（3w 实测，**515/w**） | 3w 1,471-1,510 ✓ | ~190/w（+171%） |
| H3 GET 1K | 71.6k(2w) / 107.3k(3w)，**35.8k/w** | 72-81k(2w) / 106-109k(3w) ✓ 档对档吻合 | ~32k/w（+12%） |
| H3 GET 64K | 13.3-13.9k(2w) **+ 26.6k(4w)**，**6.6-6.9k/w** | 13.1k(2w) / 19.4-19.7k(3w) ✓ 档对档吻合 | ~2.7k/w（+146%） |

全部 0 失败 / 0 丢包。**结论：H2 与 H3 的全部格档对档精确回到 #5 水平（+2~4%）——套件偏好是 #6 大体回归的唯一根因，不存在第二因子。** 4w 档 64K 实测 4 个 worker 各占 96-97% 单核——满载密封瓶颈，与 ChaCha20-Poly1305 ~600 MB/s/core 的天花板自洽（4×6,644×64KiB ≈ 1.68 GB/s ≈ 4×~430 MB/s/worker）。

> **勘误的勘误**：本节初版曾据 13.3-13.9k 的样本簇判定"H3 64K 仅恢复约四成（3.4k/w），存在第二回归因子（H3 发送路径每请求开销，嫌疑 `7de1a596`）"——**系档位误读**：未逐样本实测 worker 命中数，把 2w 档当 4w 档归一。经采样器 per-tid 分布（该簇仅 2 个 worker 烧 CPU）与 /proc jiffies（26.6k 样本 4 worker 各 97%）双法证伪。教训与 #3 以来的 H3 方法论一脉相承：**每样本必须实测档位，快速验证也不例外**；五连 2w 档在 #5/#6 的档位分布（2w 概率 ~2/3）下纯属抽样运气。

### 7.6 修复后全矩阵复测验证（#7，2026-09-26 17:07-17:55，fix `72426dc0` 同构建）

套件偏好修复落地、环境修复（libcrypto -O3）后，完整重跑 #6 矩阵（H1×3 / H2×2 / H3×6，同场景同参数同绑核；新数据 `results7/s1..s9/`，只追加）。**每个 H3 样本档位均为脚本内置实测**（`bench7.sh` 每场景前后快照 `/proc/<pid>/task/*/stat` 的 utime+stime 差值，≥300 jiffies 记为命中；lite 主线程不计——它不在 reuseport 抽奖内），彻底贯彻 §7.5 教训。

**H2 全四格恢复 #5 并反超（中位，2 样本）**：

| 格 | lite RPS | Δ#5 | Δ#6 | vs OR | vs nginx |
|---|---:|---:|---:|---:|---:|
| GET 1K | 189,444 | +6% | +19% | 1.48× | 1.95×（健康样本口径 1.68×，见注） |
| GET 64K | 22,302 | +1% | +115% | 2.55× | 1.92× |
| GET 1M | 1,828 | +3% | +149% | 3.11× | 2.24× |
| POST 1M | 915 | +7% | +157% | 3.32× | 2.41× |

注：nginx H2 1K 两样本 81.8k/112.6k 离散异常（其余 11 格对照全部 ±3% 内），中位被低样本拉低；ratio 按健康样本 112.6k 计。两对照大体格对 #5 全部 +1~+3%（环境零漂移成立）。

**H3 档对档恢复 #5（每 worker 中位；lite 6 样本全实测档，nginx 9 样本 6 实测档）**：

| 格 | lite /w | Δw#5 | Δw#6 | nginx /w | Δw#5 | lite/nginx |
|---|---:|---:|---:|---:|---:|---:|
| GET 1K | 37,074 | +3% | +16% | 25,819 | +3% | **1.44×**（#5 1.44×，#6 1.19×） |
| GET 64K | 6,734 | +4% | +149% | 3,035 | +5% | **2.22×**（#5 2.2×，#6 0.90×） |
| GET 1M | 520 | +3% | +174% | 210 | +3% | **2.48×**（#5 2.5×，#6 0.90×） |
| POST 1M | 263 | +5% | +103% | 93 | +1% | **2.83×**（#5 2.7×，#6 ~1.4×） |

lite POST 样本恰构成完美等差数列：255(1w)/530(2w)/790(3w)/1,050(4w) ≈ 263/w——档位实测与量化模型互证。H1 全 12 格对 #6 ±2%（对 #5 +1~+14% 为整机同向漂移，两对照同步）——H1 面不受影响，环境等价。全程 0 失败 / 0 非 2xx / 0 UDP 丢包（60 份 H3 json 逐一核查）。

**结论：`72426dc0` 后 H2/H3 全部格档对档回到 #5 水平（+1~+7%，与整机 +2~5% 漂移一致），#5 的全部领先比例（H3 每 worker 1.44×/2.2×/2.5×/2.8×，H2 1.5-3.3×）完整恢复——套件偏好是 #6 回归的唯一根因，全矩阵多样本+实测档位双重口径下最终定谳，无第二因子。**

**#7 过程记录**（方法论沉淀）：① 端口竞态复发一次（s3 h1 nginx 整轮 `EADDRINUSE 38080`，非 LISTEN 态源端口对 `ss -tuln` 不可见），本轮起 nginx START 前加 bind 预探（镜像 nginx 语义：SO_REUSEADDR + 0.0.0.0 TCP/UDP，探到空才启动），此后 10 轮 nginx 零失败；h1 nginx 有效样本 2 个（218.9k/217.3k，±0.4%）。② nginx 无 pid 文件（`nginx_run/logs/` 不存在）且本机 master 不重挂 init（PPid=4346 subreaper）、`pgrep -f` 自匹配——worker 档位发现改为 /proc 扫描（comm=nginx + cmdline 含 nginx_run/nginx_quic.conf 即 master，取最大 pid 防上轮残留；cat-demo 实例前缀不同天然排除），s4 起生效，s1-s3 nginx 档位以 s7-s9 补测。③ cat-demo nginx 本轮确认常驻（719126，与 #5/#6 后台负载一致）。
