# lite-nginx 与 Nginx 单机 HTTP/1、HTTP/2 压测计划

> 2026-07-19 已执行。结果、偏差和未满足的验收门槛见
> [lite_nginx_nginx_single_host_benchmark_report.md](lite_nginx_nginx_single_host_benchmark_report.md)。

## 1. 目标与结论边界

本计划用于在当前唯一可用的开发机上完成以下工作：

- 比较 lite-nginx 与仓库固定版本 Nginx 的 HTTP/1.1、HTTP/1.1 over TLS 和 HTTP/2 over TLS 性能；
- 分离协议处理、TLS、反向代理和上游连接复用的成本；
- 检查 lite-nginx 在持续负载、过载、慢客户端、异常请求和上游故障下的健壮性；
- 建立可重复执行的配置、运行、采集和报告流程，为后续性能优化提供稳定基线。

由于压测器、被测服务和上游运行在同一台 WSL2 主机上，最终结果定义为：

- **可信用途**：同机、同配置、同 CPU 预算下的相对吞吐、相对延迟、CPU/request 和内存稳定性；
- **不可信用途**：生产环境绝对容量、真实网络延迟、网卡吞吐上限和跨机连接扩展性；
- **主要判断依据**：lite-nginx/Nginx 的配对比值，而不是单次绝对 RPS。

## 2. 已确认的仓库与环境事实

### 2.1 Nginx 基线

- `scripts/build_nginx.sh:15` 固定 `nginx_version="1.31.3"`；
- 固定源码目录为 `temp/nginx-1.31.3/`；
- 固定二进制为 `temp/nginx-install/sbin/nginx`；
- 当前源码中的 `configure` 和安装后的 Nginx 二进制均已存在且可执行；
- 构建包含 HTTP/2 模块，并与项目一样使用 BoringSSL；
- Nginx 1.31.3 的 `http2_max_concurrent_streams` 默认值为 128，见
  `temp/nginx-1.31.3/src/http/v2/ngx_http_v2_module.c:336-348`。

Nginx 版本、源码和二进制必须继续以 `scripts/build_nginx.sh` 为准，禁止换用系统 Nginx 或其他缓存版本。

### 2.2 lite-nginx 基线

- 非 TLS listener 只处理下游 HTTP/1.1；
- TLS listener 通过 ALPN 协商 HTTP/1.1 或 HTTP/2；
- 反向代理上游使用 HTTP/1.1；
- `worker_processes=N` 会创建 N 个 worker event loop；
- listener 和 accept 协程运行在额外的 accept event loop；
- 接受后的连接按轮询方式分配给 worker loop；
- lite-nginx 默认广告并限制每条 HTTP/2 连接最多 128 个对端并发流，见
  `src/http/Http2Connection.h:67-77`。

因此 `worker_processes=2` 时，lite-nginx 的稳定工作线程模型是一个 accept loop 加两个 worker loop，不能只按配置中的 worker 数与 Nginx 比较 CPU 占用。

### 2.3 当前主机约束

当前 WSL2 Linux 环境看到：

- 20 个逻辑 CPU；
- 10 组 sibling，分别为 `0-1`、`2-3`、...、`18-19`；
- cgroup v2；
- systemd 正常运行；
- `taskset` 和 `systemd-run` 可用；
- 当前未安装 `h2load`、`wrk2`、`pidstat` 和 `perf`；
- 当前 `build/` 的 `CMAKE_BUILD_TYPE` 为空，不能用于正式性能结果。

WSL 暴露的 CPU 拓扑不保证与宿主机物理 P-core/E-core 一一对应。CPU affinity 只能隔离 Linux 进程使用的 vCPU，不能消除 Windows 调度、温度和电源管理噪声。

## 3. 公平性原则

### 3.1 单机顺序执行

- lite-nginx 与作为被测对象的 Nginx 不同时运行；
- 两者顺序占用相同地址、端口、CPU 集和 CPU quota；
- 公共上游在一整组 lite-nginx/Nginx 配对测试期间不重启；
- 压测器使用同一个二进制、参数和请求输入；
- 每轮执行前验证实际协商协议、状态码、响应长度和响应体哈希。

### 3.2 固定 CPU 预算

主测试统一设置：

| 角色 | Linux vCPU | CPU quota | 线程/worker |
|---|---|---:|---:|
| 被测服务 | `0,2,4` | 200% | `worker_processes=2` |
| h2load | `6,8,10,12` | 不额外限额 | 4 threads |
| 公共上游 | `14,16` | 200% | 1 至 2 workers |
| 监控和看门狗 | `18` | 不额外限额 | 1 |
| 主测试不主动使用 | 所有奇数 vCPU | - | - |

被测服务允许在三个 vCPU 上调度，但整个 cgroup 最多使用两个 CPU 的计算时间。这样 lite-nginx 的 accept loop 成本也计入同一个 200% 预算，Nginx master 和 worker 同样受此预算限制。

被测服务统一通过 transient systemd unit 启动。lite-nginx 模板：

```bash
sudo systemd-run \
  --unit=bench-lite \
  --property=AllowedCPUs=0,2,4 \
  --property=CPUQuota=200% \
  --property=CPUAccounting=yes \
  --property=MemoryAccounting=yes \
  --working-directory="$PWD" \
  ./build-bench/apps/lite_nginx \
  --config scripts/benchmark/http/configs/lite_nginx.conf
```

Nginx 模板：

```bash
sudo systemd-run \
  --unit=bench-nginx \
  --property=AllowedCPUs=0,2,4 \
  --property=CPUQuota=200% \
  --property=CPUAccounting=yes \
  --property=MemoryAccounting=yes \
  --working-directory="$PWD" \
  temp/nginx-install/sbin/nginx \
  -g "daemon off;" -p "$PWD/" \
  -c scripts/benchmark/http/configs/nginx_sut.conf
```

Nginx 必须使用 `daemon off`，使 master 和全部 worker 留在同一 cgroup。runner 在每次启动后通过 `systemctl show <unit> -p ControlGroup` 解析真实 cgroup 路径，禁止硬编码 `/sys/fs/cgroup` 下的 unit 目录。

### 3.3 配置等价

两边必须保持：

- 相同 listener 地址和端口；
- 相同 TLS 证书、私钥、TLS 版本和可比 cipher；
- 相同 `worker_processes=2`；
- 相同 `http2_max_concurrent_streams=128`；
- 相同上游地址、响应内容和响应头；
- 相同下游与上游 keepalive 目标；
- 相同连接、读取、发送超时；
- access log 关闭，error log 只保留错误；
- gzip、HTTP/3、缓存和其他非测试功能关闭；
- proxy buffering 关闭；
- Nginx 上游显式使用 HTTP/1.1 并清除 `Connection` 头。

上游连接池使用一个不会在主测试中触顶的值，例如 256。Nginx 同时将 `keepalive_requests` 设为足够大的值，避免测试中周期性关闭上游连接。lite-nginx 主结果使用 `steal auto`，另加一组 `steal off` 作为连接池分片行为诊断，不混入主排名。

### 3.4 两种结果层次

1. **主结果：同一上游反代**
   - 是 lite-nginx 与 Nginx 的主要公平对比；
   - 两边转发给完全相同的 HTTP/1.1 上游；
   - 用于比较下游协议、TLS、代理转发和连接池整体成本。
2. **辅助结果：本地直出**
   - lite-nginx 通过 `script_file`，Nginx 通过静态文件；
   - 两条路径并不等价；
   - 只表示各自产品的短路径能力，不用于解释纯 HTTP 框架差异。

## 4. 计划产物

执行本计划时增加以下基准设施，具体文件名可以在实现时微调，但职责不得合并：

```text
scripts/benchmark/http/
├── configs/
│   ├── lite_nginx.conf
│   ├── nginx_sut.conf
│   └── nginx_backend.conf
├── payloads/
│   ├── response_1k.bin
│   ├── response_64k.bin
│   └── request_64k.bin
├── run_pair.sh
├── run_matrix.sh
├── collect_cgroup.sh
├── verify_response.sh
└── summarize.py
```

原始结果写入忽略目录：

```text
temp/http-benchmark-results/<timestamp>-<git-sha>/
```

每次运行至少保存：

- Git commit、工作树状态和二进制 SHA-256；
- CMake cache、实际编译命令摘要和 Nginx `-V`；
- `uname -a`、`lscpu`、内存、内核参数、ulimit；
- CPU affinity、cgroup 参数和运行顺序；
- h2load 完整 stdout、stderr 和逐请求日志（工具支持时）；
- 被测服务 cgroup `cpu.stat`、`memory.current`、`memory.peak`、`memory.events`；
- 被测服务和上游错误日志；
- 每轮开始和结束时的 TCP 统计；
- 分析后的 CSV/JSON 和 Markdown 摘要。

最终选定基线以独立报告写入 `feature/`，原始大文件不提交 Git。

## 5. 执行阶段

### Phase 0：工具和安全检查

- [ ] 安装发行版提供的 nghttp2 客户端工具，确认 `h2load --version`；
- [ ] 保存 `h2load --help`，确认支持 `--h1`、`--alpn-list`、`--duration`、`--warm-up-time`、`--rps` 和 `--log-file`；
- [ ] 安装 `sysstat` 以获得 `pidstat`；若 WSL 内 `perf` 不可用，将其标记为可选项，不阻塞基准；
- [ ] 验证 cgroup v2、systemd、`AllowedCPUs`、`CPUQuota` 和 memory accounting；
- [ ] 将 `ulimit -n` 固定并记录，目标至少 65536；
- [ ] 检查可用内存和 swap，定义健壮性测试的显式 `MemoryMax`；
- [ ] 保留 vCPU 18 给监控和停止命令；
- [ ] 所有过载和健壮性命令必须带总超时；
- [ ] 机器接通电源，Windows 使用固定电源模式，并关闭编译、浏览器、Docker 等重负载；
- [ ] 若测试前系统负载或后台 CPU 超过阈值，则放弃该轮而不是保留异常结果。

### Phase 1：准备可比较构建

lite-nginx 使用独立 Release 构建目录：

```bash
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_BUILD_APPS=ON \
  -DFIBER_BUILD_EXAMPLES=ON \
  -DFIBER_BUILD_TESTS=OFF \
  -DFIBER_USE_JEMALLOC=OFF \
  -DFIBER_ENABLE_LTO=ON
cmake --build build-bench --target fiber_app_lite_nginx -j
```

Nginx 继续通过仓库脚本准备：

```bash
scripts/build_nginx.sh
temp/nginx-install/sbin/nginx -V
```

- [ ] 禁止使用当前未设置 build type 的 `build/apps/lite_nginx` 跑正式数据；
- [ ] 记录 lite-nginx 与 Nginx 的真实优化参数和链接库；
- [ ] 主结果使用项目生产配置：lite-nginx Release + LTO、默认 glibc allocator；
- [ ] 如需解释编译器优化差异，再增加 `LTO=OFF` 或统一优化参数的诊断构建，诊断结果不得覆盖主结果。

### Phase 2：准备公共上游和专用配置

公共上游必须支持以下固定端点：

| 端点 | 行为 |
|---|---|
| `GET /bench/1k` | 返回固定 1024 字节 body |
| `GET /bench/64k` | 返回固定 65536 字节 body |
| `POST /bench/echo` | 完整读取请求体并原样返回 |
| `POST /bench/discard` | 完整读取请求体并返回固定小响应 |
| `GET /fault/delay/<ms>` | 延迟后响应，仅用于健壮性 |
| `GET /fault/close` | 接收后提前关闭，仅用于健壮性 |
| `GET /fault/partial` | 返回部分响应后关闭，仅用于健壮性 |

实现选择顺序：

1. 优先实现一个专用、低分配、HTTP/1.1 keepalive benchmark backend；
2. GET 固定文件可以由固定版本 Nginx backend 提供；
3. POST echo 可在临时阶段复用 `http1_echo`，但只有在直压容量超过代理峰值两倍时才能进入正式结果。

- [ ] 上游固定运行在 `127.0.0.1:19001`；
- [ ] 上游绑定 vCPU `14,16`，CPU quota 200%；
- [ ] 关闭上游 access log；
- [ ] 上游响应 body 使用预生成文件或固定复用缓冲，不在每请求生成随机内容；
- [ ] 直压验证上游成功率为 100%，吞吐不低于代理预估峰值的两倍；
- [ ] 若上游 CPU 超过 50% 或延迟随代理负载明显上升，将被测服务 CPU quota 降到 100%，不能继续用已受上游限制的数据排名。

lite-nginx 专用配置要求：

- [ ] `worker_processes 2`；
- [ ] `listen 127.0.0.1:18080`；
- [ ] `listen 127.0.0.1:18443 ssl`，不启用 `http3`/`quic`；
- [ ] `access_log off`；
- [ ] `connection_pool.keepalive_size 256`；
- [ ] 主结果 `steal auto`；
- [ ] 所有测试 location 使用同一命名 upstream；
- [ ] `proxy_buffering off`；
- [ ] 不加载样例日志和无关 script location。

Nginx 被测配置要求：

- [ ] `worker_processes 2`；
- [ ] `worker_connections` 足以覆盖最大连接数，建议至少 8192；
- [ ] 使用相同的 `127.0.0.1:18080/18443`；
- [ ] TLS listener 显式 `http2 on`；
- [ ] 显式 `http2_max_concurrent_streams 128`；
- [ ] `access_log off`，`error_log` 只记录 error；
- [ ] upstream `keepalive 256`、较大的 `keepalive_requests`；
- [ ] `proxy_http_version 1.1`；
- [ ] `proxy_set_header Connection ""`；
- [ ] `proxy_buffering off`；
- [ ] gzip、cache、HTTP/3 和其他无关模块关闭。

### Phase 3：功能和协议预检

每次正式矩阵运行前执行：

```bash
curl --fail --silent --show-error \
  --http1.1 http://127.0.0.1:18080/bench/1k --output /tmp/bench-h1.bin

curl --fail --silent --show-error --insecure \
  --http1.1 https://localhost:18443/bench/1k --output /tmp/bench-h1-tls.bin

curl --fail --silent --show-error --insecure \
  --http2 https://localhost:18443/bench/1k --output /tmp/bench-h2.bin
```

- [ ] 检查 curl 报告的实际 HTTP version；
- [ ] 对状态码、Content-Length 和 body SHA-256 做断言；
- [ ] POST 64 KiB 后检查响应体哈希；
- [ ] lite-nginx 与 Nginx 的响应头集合除 `Server` 等身份字段外保持一致；
- [ ] 检查两个服务都没有新增 error log；
- [ ] 检查 FD、worker 数量和 CPU affinity 符合预期；
- [ ] 任何预检失败都禁止进入性能跑数。

### Phase 4：确定压测器和上游余量

- [ ] 先直压公共上游的 1 KiB、64 KiB 和 POST 64 KiB；
- [ ] 在 `taskset -c 6,8,10,12` 下运行 h2load；
- [ ] 确认 h2load 所在 CPU 集平均利用率低于 70%；
- [ ] 确认上游 CPU 低于 50%；
- [ ] 如果压测器成为瓶颈，将被测服务 CPU quota 降为 100% 后重做全部正式场景；
- [ ] 禁止仅通过给 h2load 增加与被测服务重叠的 CPU 来掩盖压测器瓶颈。

### Phase 5：吞吐上限测试

主矩阵：

| 编号 | 下游协议 | 方法与内容 | h2load 连接/流 |
|---|---|---|---|
| H1-P-1K | HTTP/1.1 明文 | GET 1 KiB | `-c 128 -m 1` |
| H1-P-64K | HTTP/1.1 明文 | GET 64 KiB | `-c 128 -m 1` |
| H1-P-POST | HTTP/1.1 明文 | POST 64 KiB | `-c 128 -m 1` |
| H1-T-1K | HTTP/1.1 TLS | GET 1 KiB | `-c 128 -m 1` |
| H1-T-64K | HTTP/1.1 TLS | GET 64 KiB | `-c 128 -m 1` |
| H2-T-1K | HTTP/2 TLS | GET 1 KiB | `-c 8 -m 64` |
| H2-T-64K | HTTP/2 TLS | GET 64 KiB | `-c 8 -m 64` |
| H2-T-POST | HTTP/2 TLS | POST 64 KiB | `-c 8 -m 64` |

POST case 在相同命令上增加 `-d scripts/benchmark/http/payloads/request_64k.bin`；h2load 会据此使用 POST。HTTP/1 POST 仍保持 `-m 1`，不启用 pipeline。

HTTP/1.1 示例：

```bash
taskset -c 6,8,10,12 h2load \
  --h1 \
  -t 4 -c 128 -m 1 \
  -D 60s --warm-up-time=15s \
  http://127.0.0.1:18080/bench/1k
```

HTTP/2 示例：

```bash
taskset -c 6,8,10,12 h2load \
  --alpn-list=h2 \
  -t 4 -c 8 -m 64 \
  -D 60s --warm-up-time=15s \
  https://localhost:18443/bench/1k
```

HTTP/2 额外诊断：

- [ ] `-c 1 -m 64`：单连接、单 worker 上限；
- [ ] `-c 2 -m 64`：至少为两个 worker 各提供一条连接；
- [ ] `-c 8 -m 128`：高并发流压力；
- [ ] `-w16 -W16`：使用较小流和连接窗口检查真实流控行为；
- [ ] 默认大窗口与 `-w16 -W16` 的结果分开报告。

每个正式 case：

- [ ] 预热 15 秒；
- [ ] 测量 60 秒；
- [ ] lite-nginx 与 Nginx 各运行至少 7 次；
- [ ] 每轮之间留 30 秒稳定时间；
- [ ] 保存成功请求、失败请求、超时、状态码、RPS、吞吐和延迟；
- [ ] 保存测量区间内 cgroup CPU 增量而不是进程启动以来的累计值；
- [ ] 结果必须标记是否发生 CPU throttling。

### Phase 6：延迟随负载变化

对 Phase 5 得到的稳定最大吞吐 `Rmax`，分别施加：

- 50% `Rmax`；
- 70% `Rmax`；
- 85% `Rmax`；
- 95% `Rmax`；
- 105% `Rmax`。

每档持续 5 分钟，使用 h2load `--rps` 控制目标速率。必须同时保存目标 RPS、实际发出 RPS 和完成 RPS；如果压测器没有维持目标速率，该档结果无效。

重点比较：

- p50、p95、p99、p99.9 和 max；
- 错误率与超时；
- lite-nginx/Nginx p99 比值；
- 每请求 CPU 时间；
- 从 95% 到 105% 过载时延迟和错误如何退化；
- 负载回落后是否恢复。

h2load 的延迟从实际发送请求开始计算，过载下可能出现 coordinated omission。HTTP/1 可以用 wrk2 固定吞吐结果交叉验证；HTTP/2 主结论必须同时结合目标/实际 RPS、错误率和服务端 CPU，不能只看 h2load p99。

### Phase 7：成对运行和统计

每个 case 采用平衡顺序，避免把温度和后台任务漂移全部归到某一个实现：

```text
pair 1: lite  -> nginx
pair 2: nginx -> lite
pair 3: lite  -> nginx
pair 4: nginx -> lite
...
```

或者使用固定随机种子生成等量顺序，并保存种子。

每一对计算：

```text
throughput_ratio  = lite_rps / nginx_rps
latency_ratio     = lite_p99 / nginx_p99
cpu_efficiency    = completed_requests / cpu_usage_seconds
cpu_eff_ratio     = lite_cpu_efficiency / nginx_cpu_efficiency
memory_ratio      = lite_peak_rss / nginx_peak_rss
```

统计要求：

- [ ] 以配对比值的中位数为主；
- [ ] 同时报告 min/max、MAD 或 IQR；
- [ ] 输出配对 bootstrap 95% 置信区间；
- [ ] 差异小于 5% 视为当前环境下不可区分；
- [ ] 差异在 5% 至 10% 时追加至少 7 对；
- [ ] 差异超过 10%，且至少 6/7 对方向一致，才作为可信差异；
- [ ] 单次最好结果不得进入结论。

### Phase 8：健壮性与故障注入

#### 8.1 持续与过载

- [ ] 在主结果 `Rmax` 的 70% 至 80% 下运行 2 小时 soak；
- [ ] 每分钟采集 RSS、FD、连接数、CPU、错误数；
- [ ] 从 0 突增到 150% `Rmax`，保持 10 分钟，再降到 50%；
- [ ] 记录拒绝、超时、RST、CPU throttling、内存峰值和恢复时间；
- [ ] 停止压力后继续观察 10 分钟，检查 RSS 和 FD 是否回落；
- [ ] 单机环境不将 24 小时结果作为首轮验收，避免宿主机状态漂移主导结论。

#### 8.2 HTTP/1 异常输入

- [ ] 慢请求行和慢请求头；
- [ ] 只声明 `Content-Length` 而不发送 body；
- [ ] 冲突的多个 `Content-Length`；
- [ ] `Content-Length` 与 `Transfer-Encoding` 同时出现；
- [ ] 非法 chunk size、缺失 chunk 终止和非法 trailer；
- [ ] 超大 URI、单个超大 header 和 header 数量爆炸；
- [ ] 客户端发送一半后关闭、半关闭和 RST；
- [ ] 客户端不读取大响应，验证写侧背压和内存边界。

#### 8.3 HTTP/2 异常输入

- [ ] 运行 h2spec 作为协议一致性基线；
- [ ] 并发流超过 128；
- [ ] 高频创建后立即 `RST_STREAM`；
- [ ] 非法 frame size、非法 stream id；
- [ ] HEADERS/CONTINUATION 顺序错误；
- [ ] 非法 HPACK 和超大 header block；
- [ ] stream/connection flow-control 窗口耗尽；
- [ ] 客户端停止发送 `WINDOW_UPDATE`；
- [ ] PING、SETTINGS、RST_STREAM 等控制帧风暴；
- [ ] GOAWAY 前后继续创建 stream。

#### 8.4 上游故障

- [ ] 上游拒绝连接；
- [ ] 上游延迟响应；
- [ ] 上游读取请求体后不响应；
- [ ] 上游发送部分 header/body 后关闭；
- [ ] 上游进程在负载中重启；
- [ ] 上游 keepalive 连接在池中失效；
- [ ] 故障解除后检查新请求是否恢复成功。

#### 8.5 Sanitizer

- [ ] ASan + UBSan 使用独立构建，关闭 LTO；
- [ ] sanitizer 只跑低到中并发和异常输入，不参与性能排名；
- [ ] TSan 单独构建和低并发执行；
- [ ] 每个 sanitizer 进程设置显式 cgroup `MemoryMax` 和总超时；
- [ ] 保存 sanitizer 完整日志和最小复现输入。

#### 8.6 单机保护措施

- [ ] vCPU 18 永不分配给被测服务、上游或 h2load；
- [ ] 最大 HTTP/1 连接数按 500、1000、5000 递增；
- [ ] 不直接冲击数万本地 TCP 连接，避免耗尽临时端口和拖死 WSL；
- [ ] HTTP/2 优先使用少量 connection 加大量 stream；
- [ ] 不在全局 loopback 接口上直接设置 `tc netem`；
- [ ] 如确需网络延迟/丢包，后续使用独立 network namespace + veth，结果与 loopback 主基准分开；
- [ ] 看门狗在内存或 load 超过阈值时停止测试并保留日志。

## 6. 指标与验收标准

### 6.1 性能指标

每个 case 必须报告：

- 成功 RPS、完成 RPS、失败率和状态码分布；
- p50、p95、p99、p99.9、max、TTFB；
- body bytes/s 和总网络字节；
- cgroup user/system/total CPU 时间；
- requests/CPU-second 和 CPU time/request；
- `nr_throttled`、`throttled_usec`；
- memory current/peak、RSS、minor/major faults；
- FD、TCP 连接、TIME_WAIT、RST 和重传；
- 上游 CPU、RPS 和延迟；
- h2load CPU 使用率。

### 6.2 正确性与稳定性验收

- 正常负载下状态码和响应体错误为 0；
- 85% `Rmax` 以内网络错误率不高于 0.01%；
- 2 小时 soak 无崩溃、死锁、assert、ASan/UBSan 错误；
- 稳态 RSS 无持续线性增长；
- 压力停止 10 分钟后，RSS 回到预热后基线的 110% 以内；
- FD 回到预热后基线附近，无持续泄漏；
- 过载解除后 60 秒内恢复到目标吞吐和延迟；
- malformed input 只能影响对应连接或 stream，不应导致进程退出或正常连接长期饥饿。

### 6.3 性能结论格式

最终报告至少包含：

| Case | lite RPS | nginx RPS | RPS ratio | lite p99 | nginx p99 | p99 ratio | lite req/CPU-s | nginx req/CPU-s | errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|

结论必须同时回答：

1. 相同 CPU quota 下，lite-nginx 与 Nginx 的吞吐比是多少；
2. 在 70%、85%、95% 负载下，两者 p99 如何变化；
3. HTTP/1 明文、HTTP/1 TLS、HTTP/2 TLS 的差异来自哪里；
4. 单连接 HTTP/2 是否受单 worker 限制，多连接能否线性扩展；
5. 每百万请求 CPU 时间和峰值内存分别是多少；
6. 过载、慢客户端和上游故障时是否可恢复；
7. 哪些差异超过 WSL2 噪声阈值，哪些只能视为不可区分。

绝对 RPS 和亚毫秒延迟只作为当前机器记录；代码优化前后的配对比值、CPU/request、错误和内存趋势才是持续回归基线。

## 7. 推荐实施顺序

1. 完成 Phase 0 至 Phase 3，确保工具、构建、配置和正确性可自动验证；
2. 实现公共 benchmark backend，并证明它不是瓶颈；
3. 只跑 `H1-P-1K` 与 `H2-T-1K` 两个 smoke case，验证采集和统计；
4. 跑完整 Phase 5 吞吐矩阵；
5. 选择有代表性的 1 KiB 和 64 KiB 场景执行 Phase 6；
6. 执行 2 小时 soak、过载恢复和上游故障；
7. 执行 malformed HTTP/1、h2spec、HTTP/2 流控与 RST 压力；
8. 最后运行 sanitizer；
9. 生成基线报告并固定之后的回归阈值。

在 smoke case、上游余量或采集完整性未通过前，不扩大压测矩阵，也不依据单次 RPS 开始性能优化。
