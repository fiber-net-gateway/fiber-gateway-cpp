# HTTP/2 本地分片连接池压测

客户端直接链接 `fiber_lib`，所有请求均通过 `LocalHttp2ConnectionPoolSet` 和 `Http2PooledExchange`。后端使用 `scripts/build_nginx.sh` 固定的 Nginx。设计背景见 [压测方案](../../../feature/local_http2_connection_pool_set_stress_plan.md)。

## 构建和运行

从仓库根目录执行：

```bash
cmake -S . -B build-http2-pool-release \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_ENABLE_LTO=OFF \
  -DFIBER_BUILD_APPS=OFF -DFIBER_BUILD_TESTS=OFF \
  -DFIBER_BUILD_NACOS=OFF -DFIBER_BUILD_CAT=OFF \
  -DFIBER_BUILD_PROMETHEUS=OFF -DFIBER_BUILD_HTTP_COMPRESSION=OFF
cmake --build build-http2-pool-release --target http2_pool_benchmark -j 6
python3 scripts/benchmark/http2_pool/run.py --suite smoke
```

若 `temp/nginx-1.31.3/configure` 或 `temp/nginx-install/sbin/nginx` 缺失，先运行 `cmake -S . -B build` 和 `scripts/build_nginx.sh`。Runner 另需 Python 3、OpenSSL CLI；设置 CPU 亲和性时需要 `taskset`。只绑定回环地址，默认使用 18082/18083/18084 三个端口。`--port` 可选择其他连续三个端口。

Runner 自动生成确定性二进制文件、短期测试证书及独立 Nginx 配置，执行 `nginx -t` 后启动后端。输出目录必须尚不存在，每个用例使用自己的日志、PID 和 Nginx 进程；结束或异常时清理该实例。故障注入只向本轮创建的 master/worker 发信号，不操作系统 Nginx 或防火墙。

```bash
# 每个负载场景 30s，FIFO 和跨 loop 生命周期各 1000 轮。
# 按本机 CPU 拓扑选择互不重叠的物理核。
python3 scripts/benchmark/http2_pool/run.py --suite standard \
  --client-cpus 0,2,4,6 --backend-cpus 8,10

# 重现取消隔离性，同时运行不取消的对照组。
python3 scripts/benchmark/http2_pool/run.py \
  --case cancel_pair,large_pair_control,cancel_recovery --duration-ms 30000

# 延长单项压力、重复采样；每轮使用新的进程。
python3 scripts/benchmark/http2_pool/run.py \
  --case four_loops,acquire_only --duration-ms 180000 --repeat 3

# 默认 8h；通过 --soak-seconds 调整时间，并在报告中注明实际时长。
python3 scripts/benchmark/http2_pool/run.py --suite soak
```

`smoke` 默认每个负载用例 10s、管理/FIFO 20 轮；`standard` 是 30s 的功能压力矩阵，不等于设计中的完整 3×180s 性能矩阵。所有负载轮次另有 1s warmup，soak 有 10s warmup。Runner 的单次网络故障持续 3s；重复故障可用 `--repeat`，并非每次默认注入 20 次。完整 8h/2h soak、动态 SETTINGS、所有身份键组合及网络丢包需按设计补充，不能由短测通过推断。

## 客户端接口

`build-http2-pool-release/example/http2_pool_benchmark --help` 列出全部参数。可直接连接已经启动的同类 fixture 后端：

```bash
build-http2-pool-release/example/http2_pool_benchmark \
  --host 127.0.0.1 --port 18082 --loops 4 --concurrency 128 \
  --connections 4 --total-connections 64 --streams 32 \
  --duration-ms 30000 --timeout-ms 2000 --acquire-ms 1000
```

`--concurrency`、`--total-connections` 按 loop 计算，`--connections` 按 loop/key 计算，`--rate` 为全进程目标速率。默认走 `try_acquire` 后 fallback，`--acquire-only` 可对照。`--keys` 通过不同 affinity 生成等价物理后端的独立池组，不应解读成 DNS、SNI 或客户端证书隔离已被穷尽验证。`--host` 仅接受 IP 字面量；TLS SNI 和校验名固定为 localhost，必须信任测试证书。

请求携带唯一 ID，逐条检查 HTTP/2、状态码、ID 回显、后端端口和完整 body 的每个字节。只支持约定的 small/medium/large/slow 静态数据集。`--mixed` 含约 1% 的限速 slow 请求，应显式将 `--timeout-ms` 提高到 30000 才适合作为无故障成功率测试；默认的 2s 预算用于短请求。

`--cancel-percent` 控制 header 后/首个 body 块后的提前析构；`--cancel-until-ms` 指定从负载起点算起的取消截止时间，后段继续正常读完，用于验证恢复。`--read-delay-ms` 暂停消费，`--hold-ms` 在开流前持有 Lease，均受整请求 deadline 约束。

固定 lane 数限制内存和在途请求。开环为每 lane 定时分配请求，错过的到达计为 `loadgen_rejected`，不会无限补建协程，也不会将拒绝算成功。`scheduled_p99_us` 只涵盖实际发出的请求，拒绝数必须一起读。为保证事件循环能回到 poller 更新缓存时间，每 lane 每 16 次完成或一次失败后进行 1ms 定时让出；本工具是正确性压力客户端，吞吐包含这项开销及 100% body 校验成本。

`lifecycle` 模式强制单组/单流上限为 1、每 loop 总连接为 2：先持有 Nginx slow 响应，启动 8 个排队 acquire 及一个可析构取消的挂起 connector，然后 clear，确认管理操作等 Lease 归还；最后一轮同时运行多个 shutdown 与 clear。每轮 clear 后用真实请求检查可复用。`fifo` 模式每 loop 每轮排队 100 个等待者，其中 10 个短 deadline 超时，检查余下 90 个的顺序和轮询防插队。这两种模式的工作记在管理/资源计数中，`started/success` 仅用于 load 模式。

## 结果与退出码

输出目录包含 `manifest.json`（版本、构建设置、亲和性、binary/fixture hash）、每个用例的 `command.json`、`client.jsonl`、`client.stderr`、`resources.jsonl`、Nginx 日志、`result.json`，以及总表 `results.json`。每秒状态在所属 loop 读取；最终 `drained` 必须显示连接、空闲连接、group、pending、在途和持有 Lease 全为 0。

延迟分桶为每倍区间 32 桶，percentile 给出桶上界；`latency_*` 为测量期间发起请求的终态延迟（含错误及 drain），`success_p99_us` 仅包含成功请求，`acquire_p99_us` 包括 warmup。`measured_rps` 只统计测量窗口内完成的成功请求。连接关闭错误与请求错误分别报告，不能把 `dial_error` 当请求失败次数。

退出码 0 表示所选断言通过，1 表示场景失败，2 表示参数/初始化错误。`--allow-errors` 仅容许过载/故障场景中的超时、Busy 或连接类错误，`Invalid`、内存错误等仍失败。Runner 对网络故障还检查每个 loop 恢复后的成功增长和错误停止增长；恢复窗口长度写入结果，短窗口不代表达到了设计的全部恢复 SLO。

`parallel_dials` 故意保留 128 并发/单 stream/1s acquire 预算的压力点，可能因队列超时失败；`parallel_dials_with_budget` 用相同负载和 5s acquire 预算对照。保留两者可以区分容量排队与取消隔离性等协议失败，不能为了总表全绿删除失败用例。

## Sanitizer

```bash
cmake -S . -B build-http2-pool-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFIBER_ENABLE_LTO=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-http2-pool-asan --target http2_pool_benchmark -j 6
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
python3 scripts/benchmark/http2_pool/run.py \
  --binary build-http2-pool-asan/example/http2_pool_benchmark \
  --case single_connection,cancel_pair,large_pair_control,lifecycle
```

仓库多个 build 目录共用 `temp/_deps` 下的依赖构建目录。切换 sanitizer/Release 时重新执行对应 `cmake -S/-B`，不要并发构建不同配置；已链接的客户端可在独立端口和 CPU 上同时运行。TSan 需另行构建，不能依据 ASan 通过宣称无数据竞争。

注意：BoringSSL 也位于该共享依赖目录；ASan/UBSan 构建会重编它。运行 Release 前必须重新配置并构建 Release 客户端，否则可能出现静态库含 sanitizer 符号而链接失败。建议使用串行构建，或为不同配置设置独立的 `FIBER_DEPS_DIR`/构建树。
