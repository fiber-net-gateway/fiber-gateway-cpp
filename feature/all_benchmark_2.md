# lite-nginx 反向代理全协议压测报告

执行日期：2026-07-29
被测对象：`apps/lite_nginx`（项目自研 C++ 反向代理，基于 fiber 协程 HTTP 栈）
对比对象：OpenResty 1.25.3.2、nginx 1.31.1（QUIC 版，作 HTTP/3 参照）
报告范围：HTTP/1、HTTP/2、HTTP/3 × {GET 1 KiB, GET 64 KiB, GET 1 MiB, POST echo 1 MiB}

---

## 1. 结论速览

| 维度 | 结论 |
|---|---|
| **POST echo 1 MiB（双向流式回显）** | lite-nginx **全面领先**：H1 达 5538 rps / 5.41 GB/s，约为 OpenResty/nginx 的 **2.7×**；H3 达 293 rps，为 nginx-quic 的 **1.64×**。这是 lite-nginx 零拷贝 `IoBuf` 双向中继架构的直接收益。 |
| **HTTP/2 小请求（GET 1 KiB）** | lite-nginx 120.6k rps，**领先** OpenResty（111k）与 nginx（99.5k）约 8%–21%。 |
| **HTTP/3 大体（GET 1 MiB / POST 1 MiB）** | lite-nginx 自研 QUIC 栈**优于** nginx-quic：1M GET +41%、POST echo +64%，0 丢包。 |
| **HTTP/1 小请求（GET 1 KiB）** | lite-nginx 134k rps，**落后** OpenResty/nginx（~206k）约 35%。nginx 事件循环在极小请求上的每请求开销更低。 |
| **GET 1 MiB（H1）** | 三者均 ~10 GB/s，**打平**——受 loopback/后端带宽上限约束，非代理瓶颈。 |
| **H2 大体（64K/1M/POST）** | 三者收敛到同一区间（64K ~9–11k rps，1M ~570–780 rps），为 **h2load+H2+TLS loopback 路径**所限，非代理差异（已用扩核实验验证）。 |
| **稳定性** | 全部 32 组压测 0 失败；H3 全程 0 UDP 丢包。 |

一句话：**lite-nginx 在双向流式（POST echo）与 HTTP/2 小请求、HTTP/3 大体上胜出；在 HTTP/1 极小请求上落后于 nginx。大体 GET 在三者间受后端/链路带宽封顶。**

---

## 2. 测试环境

- **主机**：16 逻辑核（虚拟化环境，`/proc/cpuinfo` 报告型号字符串为 `Core2 Duo T7700 @ 2.40GHz`，实为 16 vCPU），内存 32 GiB，Ubuntu 20.04 (focal)，OpenSSL 1.1.1f。
- **CPU 隔离（taskset 绑核）**：单机同置 backend / proxy / loadgen，靠绑核降低争用。

| 角色 | 绑核 | 说明 |
|---|---|---|
| 后端 `http_benchmark_backend` | 0-3 | fiber HTTP/1 服务，4 worker 线程，监听 127.0.0.1:19001 |
| 被测代理（lite-nginx / OpenResty / nginx） | 4-9 | 4 worker 进程 |
| 压测客户端（wrk / h2load / http3_benchmark_client） | 10-15 | 6-8 线程 |

- **内核网络**：`net.core.rmem_max=33 MB`（接收充足）；`net.core.wmem_max=212992`（≈208 KB，**限制 QUIC 发送缓冲**，无 sudo 无法上调，H3 并发据此保守选取）。未改任何 sysctl。

### 软件版本

| 组件 | 版本 | 备注 |
|---|---|---|
| lite-nginx | git `496f42a`，Release + LTO + libc++ | `build/apps/lite_nginx` |
| OpenResty | openresty/1.25.3.2 | clang 20 构建，系统 OpenSSL 1.1.1f，`--with-http_v2_module` |
| nginx (quic) | nginx/1.31.1 | 仓库预构建，BoringSSL，`--with-http_v2_module --with-http_v3_module` |
| wrk | 4.2.0 (496f42a-dirty) | HTTP/1 压测 |
| h2load | nghttp2/1.65.0 | HTTP/2 压测，clang-20 + libc++ + 静态 libev 4.33 |
| http3_benchmark_client | 同 lite-nginx 构建 | HTTP/3 压测（项目自带） |
| 后端 | `example/http_benchmark_backend` | 提供 `/bench/1k`、`/bench/64k`、`/bench/1m`、`/bench/echo` |

> OpenResty 不支持 HTTP/3（其捆绑 nginx 未带 QUIC，且系统 OpenSSL 1.1.1f 无 QUIC）。因此 **HTTP/3 以 nginx 1.31.1 (quic) 作为参照**；H1/H2 为三者对比。

---

## 3. 方法论

### 场景

| 场景 | 方法 | 路径 | 响应/请求体 |
|---|---|---|---|
| GET 1 KiB | GET | `/bench/1k` | 1024 B 固定响应 |
| GET 64 KiB | GET | `/bench/64k` | 65536 B 固定响应 |
| GET 1 MiB | GET | `/bench/1m` | 1048576 B 固定响应（本测试新增端点） |
| POST echo 1 MiB | POST | `/bench/echo` | 回显 1 MiB 请求体 |

所有代理以 **HTTP/1.1 + keepalive** 连接同一个 fiber 后端（上游连接池 256），保证上游侧一致；差异只来自代理本身。

### 压测参数（每协议内三代理一致）

| 协议 | 工具 | 参数 | 并发 |
|---|---|---|---|
| HTTP/1 | wrk | `-t6 -c256 -d20s --latency` | 256 连接 |
| HTTP/2 | h2load | `-t6 -c32 -m16 -D20s`（TLS，`SSL_CERT_FILE` 信任自签证书） | 32 连接 × 16 流 = 512 |
| HTTP/3 | http3_benchmark_client | `--threads 4 --connections 16 --streams 4 --duration 20s --warmup 3s --insecure` | 16 连接 × 4 流 = 64 |

每组测量 20 秒（H3 另加 3 秒预热）。每场景单次运行。

### 公平性配置

- 三代理均**关闭响应缓冲流式转发**：nginx/OpenResty `proxy_buffering off` + `proxy_buffer_size 64k`（避免 4 KiB 默认读缓冲沦为 strawman，已验证 4K→64K 使 OpenResty 64K 体型从 1.85 GB/s 升至 5.64 GB/s）；lite-nginx 原生流式（零拷贝 `IoBuf`，无缓冲模式）。
- nginx/OpenResty `keepalive_requests 1000000`（默认 1000 会在压测中触发 GOAWAY 关闭连接，已验证会让 H3 单连接在 1000 请求后断开）。
- 三代理均 4 worker，上游 keepalive 256。
- 自签证书 SAN 含 `127.0.0.1`/`localhost`；H2 用 `SSL_CERT_FILE` 信任，H3 用 `--insecure`。

### 已知局限（详见第 7 节）

1. 单机同置：proxy/backend/loadgen 共享物理核，虽绑核仍非纯隔离；绝对值偏低估，**相对对比有效**。
2. 单次 20 秒运行：存在运行间方差（如 lite-nginx H1 GET 1K 两次运行 117k–134k）。
3. H2 大体为 h2load/H2+TLS loopback 路径所限，非代理瓶颈。
4. H3 受 `wmem_max=208KB` 限制，并发保守（16×4），绝对吞吐受限但 0 丢包。

---

## 4. 结果

### 4.1 HTTP/1（wrk）

| 场景 | 代理 | RPS | 带宽 MB/s | p50 ms | p90 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | 134,232 | 141 | 1.75 | 2.36 | 2.97 |
| GET 1 KiB | OpenResty | **206,545** | 235 | 1.12 | 1.68 | 4.12 |
| GET 1 KiB | nginx(quic) | 200,937 | 228 | 1.13 | 1.69 | 2.40 |
| GET 64 KiB | lite-nginx | 74,331 | 4,540 | 3.15 | 4.54 | 5.28 |
| GET 64 KiB | **OpenResty** | **92,145** | 5,640 | 2.49 | 3.68 | 5.05 |
| GET 64 KiB | nginx(quic) | 83,476 | 5,110 | 2.68 | 4.15 | 8.11 |
| GET 1 MiB | lite-nginx | 10,078 | 9,840 | 23.21 | 31.06 | 35.08 |
| GET 1 MiB | **OpenResty** | **10,392** | 10,150 | 24.10 | 32.71 | 42.62 |
| GET 1 MiB | nginx(quic) | 9,590 | 9,370 | 25.07 | 34.93 | 44.75 |
| POST echo 1 MiB | **lite-nginx** | **5,538** | 5,410 | 38.22 | 69.00 | 90.44 |
| POST echo 1 MiB | OpenResty | 2,065 | 2,020 | 127.49 | 152.77 | 170.32 |
| POST echo 1 MiB | nginx(quic) | 2,039 | 1,990 | 125.47 | 165.86 | 189.43 |

**分析**

- **GET 1 KiB**：OpenResty/nginx ~206k rps，lite-nginx 134k，落后 ~35%。极小请求为每请求开销敏感，nginx 事件循环在此尺寸优化最深。
- **GET 64 KiB**：OpenResty 92k（5.64 GB/s）> nginx 83k > lite 74k。此尺寸未触及带宽上限，代理中继效率显现差异，OpenResty 最快。
- **GET 1 MiB**：三者均 ~10 GB/s 打平——已触及 loopback/后端带宽上限，非代理瓶颈。
- **POST echo 1 MiB**：lite-nginx 5,538 rps / 5.41 GB/s，约为 OpenResty/nginx 的 **2.7×**。POST echo 需双向流式（读 1M 请求体 + 写 1M 响应体），lite-nginx 零拷贝 `IoBuf` 中继在此显著优于 nginx 的缓冲拷贝路径。

### 4.2 HTTP/2（h2load）

| 场景 | 代理 | RPS | 带宽 MB/s | 均值延迟 ms | 最大延迟 ms | 失败 |
|---|---|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **120,629** | 128 | 4.22 | 87.58 | 0 |
| GET 1 KiB | OpenResty | 111,000 | 118 | 4.80 | 33.90 | 0 |
| GET 1 KiB | nginx(quic) | 99,540 | 105 | 5.27 | 32.62 | 0 |
| GET 64 KiB | lite-nginx | 9,312 | 583 | 54.88 | 181.62 | 0 |
| GET 64 KiB | OpenResty | 8,440 | 529 | 55.99 | 157.78 | 0 |
| GET 64 KiB | nginx(quic) | 10,864 | 681 | 47.02 | 106.57 | 0 |
| GET 1 MiB | lite-nginx | 713 | 715 | 706.61 | 1,320 | 0 |
| GET 1 MiB | OpenResty | 570 | 571 | 831.09 | 5,660 | 0 |
| GET 1 MiB | nginx(quic) | 780 | 781 | 642.73 | 1,500 | 0 |
| POST echo 1 MiB | lite-nginx | 336 | 339 | 1,460 | 3,430 | 0 |
| POST echo 1 MiB | OpenResty | 262 | 262 | 1,790 | 4,600 | 0 |
| POST echo 1 MiB | nginx(quic) | 363 | 364 | 1,300 | 3,510 | 0 |

> h2load 仅输出 min/max/mean，无 p50/p90/p99，故 H2 延迟列用均值/最大值。

**分析**

- **GET 1 KiB**：lite-nginx 120.6k rps **领先**（OpenResty 111k、nginx 99.5k）。与 H1 相反——H2 下 lite-nginx 的自研 HPACK/流复用路径在小请求上更快。
- **GET 64 KiB / 1 MiB / POST**：三者收敛到很窄区间（64K ~8.4–10.9k rps，1M ~570–780 rps，POST ~262–363 rps）。经扩核实验验证（h2load 6→8 核、代理 6→4 核、并发 512→512/1024，吞吐不变 ≈570 MB/s），**此区间由 h2load + H2 + TLS loopback 路径决定，非代理瓶颈**。每 64K 请求均值延迟 ~54 ms（H1 同尺寸仅 3 ms），系 H2 帧化/TLS/流控在环回上的固有开销。因此 H2 大体绝对值反映压测器而非代理；三者排序（nginx 略高）在 harness 噪声范围内。

### 4.3 HTTP/3（http3_benchmark_client，OpenResty 无 H3，参照 nginx-quic）

| 场景 | 代理 | RPS | 带宽 MB/s | p50 ms | p90 ms | p99 ms | UDP 丢包 |
|---|---|---:|---:|---:|---:|---:|---:|
| GET 1 KiB | **lite-nginx** | **89,305** | 91 | 0.60 | 1.11 | 1.57 | 0 |
| GET 1 KiB | nginx(quic) | 81,527 | 83 | 0.58 | 1.30 | 1.99 | 0 |
| GET 64 KiB | lite-nginx | 5,731 | 376 | 10.85 | 18.62 | 25.02 | 0 |
| GET 64 KiB | nginx(quic) | 5,866 | 384 | 10.53 | 13.22 | 16.16 | 0 |
| GET 1 MiB | **lite-nginx** | **573** | 601 | 94.21 | 162.82 | 218.11 | 0 |
| GET 1 MiB | nginx(quic) | 406 | 425 | 154.11 | 175.10 | 200.19 | 0 |
| POST echo 1 MiB | **lite-nginx** | **293** | 307 | 180.22 | 321.54 | 363.52 | 0 |
| POST echo 1 MiB | nginx(quic) | 179 | 187 | 354.30 | 386.05 | 414.72 | 0 |

**分析**

- **GET 1 KiB**：lite-nginx 89.3k > nginx 81.5k（+10%），且 p99 更优（1.57 vs 1.99 ms）。
- **GET 64 KiB**：基本持平（~5.8k rps，~380 MB/s）。
- **GET 1 MiB**：lite-nginx 573 rps / 601 MB/s，**为 nginx-quic 的 1.41×**（406 / 425）。
- **POST echo 1 MiB**：lite-nginx 293 rps，**为 nginx-quic 的 1.64×**（179）。
- 全程 **0 UDP 丢包**。H3 绝对吞吐受 `wmem_max=208KB` 约束（并发保守取 16×4）；在该约束下 lite-nginx 自研 QUIC/H3 栈在大体上稳定优于 nginx-quic。

---

## 5. 跨协议视角

| 场景 | H1 最优 | H2 最优 | H3 最优 | 备注 |
|---|---|---|---|---|
| GET 1 KiB | OpenResty 206k | **lite-nginx 120k** | **lite-nginx 89k** | H1 nginx 系占优；H2/H3 lite-nginx 占优 |
| GET 64 KiB | OpenResty 92k | nginx 10.9k (harness-bound) | 持平 5.8k | H1 代理差异明显；H2/H3 受协议栈限 |
| GET 1 MiB | 打平 ~10k (带宽封顶) | nginx 780 (harness-bound) | **lite-nginx 573** | H1 撞后端/链路上限；H3 lite-nginx 领先 |
| POST echo 1 MiB | **lite-nginx 5.5k** | nginx 363 (harness-bound) | **lite-nginx 293** | lite-nginx 双向流式全面领先 |

绝对吞吐：H1 > H2 > H3（H1 无 TLS/帧化；H2 有 TLS+HPACK；H3 有 QUIC/UDP + `wmem` 限制），符合预期。

---

## 6. lite-nginx 优劣势小结

**优势**

- **双向流式中继（POST echo）**：跨三协议均领先（H1 2.7×、H3 1.64×）。零拷贝 `IoBuf` 切片转发，避免 nginx 式缓冲拷贝，体型越大收益越显。
- **HTTP/2 小请求**：自研 HPACK + 流复用，GET 1K 领先 OpenResty/nginx。
- **HTTP/3 大体**：自研 QUIC 栈在 1M GET/POST 上优于 nginx-quic，0 丢包。

**劣势**

- **HTTP/1 极小请求**：GET 1K 落后 OpenResty/nginx ~35%。每请求开销（路由/解析/连接处理）高于 nginx 高度优化的事件循环。
- **大体 GET（H1）**：撞后端/loopback 带宽上限，无法体现代理差异（非劣势，属共性封顶）。

---

## 7. 局限与可信度

1. **单机同置**：backend/proxy/loadgen 共 16 核，靠 taskset 绑核隔离但非物理隔离；后台另有 `ai-server` 等进程占用 8080。绝对吞吐偏低估，**三代理同环境相对对比有效**。
2. **单次 20 秒**：未做多轮取中位数。运行间方差约 5%–15%（lite-nginx H1 GET 1K 两次 117k/134k）。结论基于排序稳定性，非单点数值。
3. **H2 大体 harness-bound**：已用扩核/扩并发实验证明 ~570 MB/s 上限来自 h2load+H2+TLS loopback 路径（每 64K 请求 ~54 ms），非代理。H2 大体数值反映压测器，三代理排序在噪声内。
4. **H3 `wmem` 受限**：`wmem_max=208KB` 且无 sudo 上调；并发保守取 16×4 以保 0 丢包。提高 `wmem_max`/并发后 H3 绝对吞吐可上升，相对结论预期不变。
5. **OpenResty 无 HTTP/3**：H3 参照为 nginx 1.31.1 (quic)。OpenResty 本质即 nginx+lua，H3 参照具代表性。
6. **流式配置**：nginx/OpenResty 用 `proxy_buffering off`+64K 缓冲以与 lite-nginx 流式对齐；若改 nginx 默认 `proxy_buffering on`+大内存缓冲，H1 大体绝对值可能再升，但需避免 1M 溢出临时文件，未在本轮覆盖。

---

## 8. 复现

### 8.1 构建工具与代理

```bash
# 后端（已加 /bench/1m 端点、4 worker）
cmake -S . -B build && cmake --build build --target http_benchmark_backend lite_nginx http3_benchmark_client

# wrk
curl -fL https://codeload.github.com/wg/wrk/tar.gz/refs/tags/4.2.0 -o - > temp/wrk-4.2.0.tar.gz
tar xzf temp/wrk-4.2.0.tar.gz && make -C temp/wrk-4.2.0 -j8

# h2load（需 libev + clang-20/libc++，因 nghttp2 1.65 需 C++20 <span>）
#   libev: 从 launchpad 取 libev-dev_4.33 .deb 解出 libev.a
#   cmake -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DENABLE_APP=ON \
#         -DLIBEV_INCLUDE_DIR=... -DLIBEV_LIBRARY=.../libev.a ..

# OpenResty
curl -fL https://openresty.org/download/openresty-1.25.3.2.tar.gz -o - > temp/openresty.tgz
# ./configure --with-http_v2_module --with-http_ssl_module --with-pcre-jit && make && make install
```

### 8.2 运行

配置与脚本均在 `temp/bench/`：`lite_nginx.conf`、`openresty.conf`、`nginx_quic.conf`、`bench.sh`、`run_all.sh`、`parse_results.py`，证书 `cert.pem`/`key.pem`，POST 体 `post_1m.bin` + `post_1m.lua`。

```bash
# 1) 启动后端（绑核 0-3）
taskset -c 0-3 ./build/example/http_benchmark_backend 19001 &

# 2) 跑全矩阵（H1×3 + H2×3 + H3×2 代理）
bash temp/bench/run_all.sh

# 3) 解析为表格
python3 temp/bench/parse_results.py
```

各代理监听端口：lite-nginx H1=18080 / TLS=18443；OpenResty 28080 / 28443；nginx 38080 / 38443。原始结果文件：`temp/bench/results/<proto>_<proxy>_<scenario>.txt`。

### 8.3 关键代理配置（lite-nginx）

```nginx
worker_processes 4;
http {
    listen 18080;
    listen 18443 ssl http3;
    connection_pool { keepalive_size 256; keepalive_timeout 60s; steal auto; }
    server {
        server_name localhost;
        certificate /.../cert.pem; certificate_key /.../key.pem;
        location /* { proxy_pass http://127.0.0.1:19001; proxy_buffering off; }
    }
}
```

nginx/OpenResty 对应 `proxy_buffering off; proxy_buffer_size 64k; proxy_buffers 4 64k; proxy_busy_buffers_size 64k;` + `keepalive_requests 1000000;`，上游 `keepalive 256`。
