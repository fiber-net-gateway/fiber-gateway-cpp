# 已定位：H3 客户端 UDP 临时端口碰撞导致握手超时

日期：2026-09-13。状态：**同类故障已自然复现并定位，已实施客户端最小修复**。排查基线 `499f7e0`。

**排查结论：客户端继承 `UdpBindOptions::reuse_addr = true`，不同 worker 的 `bind(0)` 可以获得相同 UDP 端口，导致回包进入另一个 endpoint，握手超时。第 6 节记录自然复现、受控实验和修复建议。原始失败没有端口记录，无法追溯确认每一次历史失败。**

第 1–5 节保留原始问题记录，其中关闭计数与 JSON 的解释已在第 6 节纠正。相关背景见 `feature/epoll_et_design.md` §14.7（已修复的 keep-alive 伪唤醒问题）。

## 1. 现象

同机 H3 压测（`example/http3_benchmark_client`，16 线程 / 16 连接 / 每连接 4 流，代理 `apps/lite_nginx` 4 worker）在**客户端刚启动、16 个 QUIC 连接同时握手**时，偶发其中一个连接握手失败，客户端输出：

```
worker 2 setup failed in connect: timed_out http3_phase=quic quic_phase=timeout tls_verify_result=0 tls_alert=0
```

进程退出码 1（其余 15 个连接正常完成，JSON 里 `failed=0 warmup_errors=0`）。客户端 `--handshake-timeout` 默认 10s，即该连接 10 秒内没有完成 QUIC 握手。

频率：`/bench/echo`（POST 1 MiB）连续 60 次 3s 压测出现 1 次；之前在 `/bench/echo` 与 `/bench/1m` 的交替 A/B 中各见过 1 次。修改前（LT）程序与 ET 程序都跑过几十次，只在 ET 程序上观察到，但样本太少，不能据此归因于 ET。

另外有一种**可能相关**的现象只出现过 2 次（ET 程序，均在 `/bench/1m` GET 预热阶段）：一个连接的 4 个流在 `body_read` 阶段同时得到 `canceled`，代理 access log 没有对应的 502 / `upstream_error`。当时没有 QUIC 关闭原因打点，无法确认是否同一问题。`bcb57b4` 之后 60 次 echo 未再出现该形态，但 1m 场景未专门复测。

## 2. 已掌握的证据

握手超时那次运行（临时在 `QuicConnection::enter_closing` 打点，打印 role/state/source/error）：

- 代理侧整个批次 60 轮 × 16 = 960 个连接，只记录到 **959** 次服务端关闭，即失败的那个连接在服务端**从未创建**（没有 Local/Peer/IdleTimeout 任何关闭记录，也没有握手阶段的 crypto error 关闭）。
- 客户端侧对应记录只有自己超时后的 `close role=client state=1 source=1 kind=0 err=0 immediate=1 streams=0`。
- 同批次里另有 1 次服务端 `err=259`（0x103 `H3_STREAM_CREATION_ERROR`，`streams=0`）关闭，未与失败运行对应上，也未追。
- 客户端统计：`dropped_datagrams=0 recv_storage_rejected=0`，说明客户端 endpoint 没有因缓冲丢包。服务端 `QuicUdpEndpoint` 的 `dropped_datagram_count_`、`rate_limited_stateless_response_count_` 目前没有暴露到任何日志或统计，**无法判断服务端是否收到并丢弃了 Initial**。

已排除：

- 不是 keep-alive 伪唤醒 / 上游死连接问题（那个问题表现为 `broken_pipe` 且 access log 有 502，已修复）。
- 不是 Retry / 地址验证限速：`Http3ServerOptions::retry` 默认 false，`validate_initial_address` 对无 token 的 Initial 直接放行，不会走 `allow_stateless_response` 限速。
- 不是版本协商：客户端与服务端均为 QUIC v1。
- 与 `steal auto/off` 无关（两种配置都见过）。

## 3. 可疑方向（未验证）

1. **UDP 源端口复用与前一轮连接残留**。每轮压测结束后客户端关闭 16 个连接（发送 `CONNECTION_CLOSE`，服务端进入 draining），约 0.2s 后新一轮客户端启动，内核可能把新 socket 绑到刚释放的源端口。服务端此时若仍向该四元组发送旧连接的 CONNECTION_CLOSE 重传或 stateless reset，新客户端会收到 DCID 不匹配的包；需要检查 `QuicUdpEndpoint::handle_peer_stateless_reset` 与客户端对未知短包的处理是否可能误判自己的握手中连接（例如把它当作对本连接的 stateless reset 而静默放弃后续 Initial 重传，但又不报错直到 10s 超时）。
2. **服务端接收路径在批量握手时丢包**。16 个 1200 B Initial 同时到达 4 个 reuseport socket，`pump_receive` 有 `max_recv_datagrams_per_wakeup` / `max_recv_bytes_per_wakeup` 预算，预算耗尽走 `post_next` 下一轮继续；理论上不丢，但 `dropped_datagram_count_` 的各个分支（`validate_initial_address` 失败、`generate_connection_id` 冲突、`find_connection` 后的状态判断等）都是静默 `return std::unexpected(...)`，需要计数确认。
3. **客户端 Initial 重传**。10s 内客户端应按 PTO 多次重传 Initial；若服务端只是丢了第一个包，重传应能建连。要确认客户端在握手阶段确实重传了（`pto_count` 是聚合值，无法区分连接），以及服务端对重传的 Initial（同 DCID）是否因某种"已见过/已拒绝"状态再次丢弃。
4. WSL2 loopback UDP 本身丢包：概率低但不能排除；裸机复现一次即可排除。

## 4. 复现与工具

准备：构建 `lite_nginx`、`http_benchmark_backend`、`http3_benchmark_client`；`temp/et-validation/` 下有本次使用的脚本与配置（未纳入版本控制）：

- `temp/et-validation/h3repro.py <before|after> <runs> [duration_s] [warmup_s]`：启动后端（CPU 0–3）与代理（CPU 4–9），对同一个代理实例重复运行 H3 客户端（CPU 10–15），打印每轮 `rps / warmup_errors / failed / io 错误分布`，失败轮保留客户端输出 `h3repro-<variant>-<i>.txt` 和当时的代理日志副本 `h3repro-<variant>-<i>-proxy.log`。
  - `H3_SCEN=echo|1m` 选择场景；`PROXY_CONF=...` 指定代理配置；变量 `after` 指 `build/apps/lite_nginx`，`before` 指 `temp/et-validation/lite_nginx-baseline`（修改前程序）。
- `temp/et-validation/proxy-access.conf` / `proxy-access-stealoff.conf`：在 `temp/et-validation/proxy.conf` 基础上打开 access log（`hv/method/path/status/outcome/up_status/up_err/t`），后者额外 `steal off`。

本次触发命令（60 轮约 5 分钟）：

```bash
PROXY_CONF=temp/et-validation/proxy-access-stealoff.conf H3_SCEN=echo \
python3 temp/et-validation/h3repro.py after 60 2 1
```

建议的下一步打点（都是临时代码，本次已移除）：

- `QuicConnection::enter_closing`：打印 role、state、source、frame_kind、error_code、immediate、`streams_.size()`（本次用过，能区分"服务端从未建连"）。
- `QuicUdpEndpoint::process_datagram` 每个 `++dropped_datagram_count_` 分支：打印分支名与 `datagram.peer`；把 `dropped_datagram_count_`、`rate_limited_stateless_response_count_` 定期输出，或作为代理统计项长期暴露。
- 客户端 `Http3ClientConnection` 握手路径：记录 Initial 发送/重传次数与收到的第一个服务端包类型；`handle_peer_stateless_reset` 命中时打印。
- 失败时同时抓 `ss -uanp | grep 28443` 或 tcpdump loopback UDP，确认源端口是否与上一轮重合、服务端是否有回包。

## 5. 影响评估

只影响压测启动时的建连，不影响已建立连接的数据正确性；生产中表现为个别客户端首个连接需等待握手超时后重试。优先级低于已修复的两个问题，但由于服务端完全没有记录，建议至少先把 endpoint 丢包计数暴露出来，避免下次仍然只能看到客户端超时。

## 6. 2026-09-13 排查结果

### 6.1 根因及代码链路

1. `include/fiber/net/UdpSocket.h` 的 `UdpBindOptions` 默认 `reuse_addr = true`、`reuse_port = false`。
2. `example/http3_benchmark_client.cpp` 的 `BenchmarkWorker::setup()` 每个 worker 创建独立 `QuicUdpEndpoint`，绑定 `0.0.0.0:0`（IPv6 为 `[::]:0`），没有覆盖该选项。
3. `src/net/detail/DatagramFd.cpp` 的 `configure_packet_options()` 在 bind 前设置 `SO_REUSEADDR`。本机实验确认：这种设置下，Linux 可以为不同、同时存活的 UDP socket 分配相同临时端口；`bind(0)` 不保证独占。
4. 两个 endpoint 向同一服务端发送时共用源地址/端口。UDP socket 分发不理解 QUIC CID，回包可能进入另一个 endpoint。`QuicUdpEndpoint::process_datagram()` 按 DCID 查连接，查不到且客户端未开启 server admission 时直接丢弃，不能交给另一个 worker。
5. 受影响连接收不到自己的握手响应。PTO 继续发包也无法修复 socket 分发关系，最终达到默认 10 秒握手超时。

因此应修正原假设：问题可以发生在**同一轮、同时存活的客户端 socket 之间**，不需要上一轮 draining 连接，不需要 ET 丢唤醒，也不需要 stateless reset 误判。

Linux 对 `SO_REUSEADDR` 允许复用的说明见 [socket(7)](https://man7.org/linux/man-pages/man7/socket.7.html)。这里关于自动临时端口碰撞的结论来自下面的本机实验，不能只靠该手册推导。

### 6.2 自然复现证据

环境：Linux `6.6.114.1-microsoft-standard-WSL2`，临时端口范围 `32768–60999`。

使用原有 `build/example/http3_benchmark_client`，保持 16 threads / 16 connections / 4 streams，通过 `LD_PRELOAD` 包装 `bind()`：默认模式只在成功绑定后调用 `getsockname()` 打印 fd/port，**不修改地址、端口或 socket 选项**。

第 130 轮（从 0 编号为 129）出现：

```text
TRACE_BIND fd=35 port=47348
TRACE_BIND fd=36 port=54401
TRACE_BIND fd=37 port=50195
TRACE_BIND fd=38 port=50195
...
worker 2 setup failed in connect: timed_out http3_phase=quic quic_phase=timeout tls_verify_result=0 tls_alert=0
```

本轮返回码 1；前 129 轮均返回 0，且每轮记录的 16 个端口各不相同。这是自然分配端口下的同轮碰撞与同形握手超时，不是强制同端口实验。

复现客户端命令：

```bash
LD_PRELOAD="$PWD/temp/h3-timeout-investigation/trace_bind.so" \
  build/example/http3_benchmark_client https://127.0.0.1:28543/bench/1m \
  --insecure --threads 16 --connections 16 --streams 4 \
  --duration 10ms --warmup 0s
```

该端口的代理使用原 4 worker 配置，后端端口改为 `29101`，HTTP 监听改为 `28180`，H3 监听改为 `28543`。自然复现期间还运行了独立压测，因此本结果不用于估计原 16 连接场景的发生概率。

本次本地证据（`temp/` 未纳入版本控制）：

- `temp/h3-timeout-investigation/trace_bind.c`、`trace_bind.so`：只读 bind 跟踪器；设置 `H3_EXCLUSIVE_PORT=1` 时才额外在 bind 前关闭 `SO_REUSEADDR`，用于对照。
- `temp/h3-timeout-investigation/traced/run-129.log`：完整失败轮 fd/port 及错误。
- `temp/h3-timeout-investigation/traced/result.log`：逐轮端口和返回码。
- `temp/h3-timeout-investigation/fast/proxy.conf`：独立代理配置。

### 6.3 受控实验与排除项

- **强制碰撞**：仅对客户端的 IPv4 `bind(0)` 改为端口 `38445`，两个 worker 的 bind 均成功，随后 worker 0 报相同 `quic_phase=timeout`。证据：`forced-port.log`、`force_port.c`。
- **丢弃首个 Initial**：Python UDP relay 仅丢首个客户端数据报，其余双向转发。客户端在约 `0.998s` 发出两个 1200 B PTO 探测，服务端回 ACK，随后客户端重传握手数据；约 `1.001s` 已交换 Handshake/应用包，最终返回码 0。PTO 虽然先排入 PING，收到 ACK 后仍能通过丢包检测补发 CRYPTO，不能据“PTO 只发 PING”认定握手恢复失效。证据：`drop_initial.py`、`drop_initial.log`。
- **关闭复用对照**：同一客户端二进制、16/16/4，只由 bind 包装器在绑定前设置 `SO_REUSEADDR=0`，连续 150 轮、2400 个连接全部通过，所有轮次均无重复端口。该结果验证最小修复方向，有限轮次全绿不代表其他故障不存在。证据：`exclusive/result.log`。
- **常规原配置**：echo 场景，16/16/4，100 轮，每轮 1s、无预热，全部通过。低频故障需要记录实际端口，单靠短批次全绿不能排除。
- **stateless reset**：`handle_peer_stateless_reset()` 要求收到包尾的 16 字节 token 与已登记 token 完整匹配。未知 DCID 本身不会导致任意握手连接进入 draining；没有发现原假设中的“未知短包自动命中当前连接”行为。

### 6.4 原始证据的两处纠正

**959 次关闭不等于只创建过 959 个连接。** `process_datagram()` 对刚创建但包处理失败的连接调用 `force_detach_connection()`；后者直接 `mark_closed()` 并 detach，不经过 `enter_closing()`。因此只统计 `enter_closing` 不能得出“服务端从未创建”，也不能据此排除回包路由错误。历史失败没有创建计数和端口信息，不能补造其具体服务端生命周期。

**建连失败轮的 JSON 可能属于旧运行。** 客户端 `main()` 检查 `setup_failed` 后直接 `return 1`，发生在汇总与写 JSON 之前。原 `h3repro.py` 固定复用 `h3repro-after-<i>.json`，运行前没有删除；执行失败后仍然 `if jf.exists(): read_text()`，会读到旧批次相同编号的 JSON。因此原先“其余 15 个连接完成、failed=0、warmup_errors=0、endpoint dropped=0”的统计不能作为这次失败的证据。当前实现会等待全部 setup 结束，任何一个失败就终止本轮，不开始正常压测。

### 6.5 最小修复与范围

已在 H3 压测客户端创建 endpoint 时显式设置：

```cpp
endpoint_options.udp.reuse_addr = false;
```

保持 `reuse_port = false`。客户端各 worker 需要独占本地端口；同一 endpoint 内的多个 QUIC 连接仍可正常共用一个 socket。服务端多 worker 的 reuseport 配置是另一种用途，不应一并关闭。

还应在 setup 错误行打印 `worker->local_address()`，并让复现脚本在每轮开始前删除旧 JSON。更广泛的 `UdpBindOptions` 默认值调整需审查 DNS、其他 UDP 客户端及服务端调用点，不能只修压测后就声称库的所有客户端都已安全。

`QuicUdpEndpoint` 类注释已说明：服务端接入与客户端出站连接建议使用不同实例，分别配置 UDP 绑定策略；客户端关闭地址/端口复用，服务端按监听与 worker 配置决定是否复用。

未把另外两次 `body_read=canceled` 或独立的 `0x103` 关闭归为同一原因；它们仍缺少对应证据。

### 6.6 最小修复验证

- `./format_code.sh`、`cmake --build build -j 4`、`git diff --check` 通过。
- `ctest --test-dir build -j 4 --output-on-failure`：2009 项中 2003 通过、4 项外部互操作测试跳过、2 项脚本响应测试失败。`ctest --test-dir build --rerun-failed --output-on-failure` 两项均通过。两项测试使用同一个 `/tmp/lite_nginx_script_result_test.js`，存在并行覆盖风险；本次未修改相关测试。
- 修复后的二进制未使用 `LD_PRELOAD`，echo 场景 16 threads / 16 connections / 4 streams，150 轮、2400 个连接全部通过；逐轮核对客户端输出，16 个端口均不重复。命令：

```bash
PROXY_CONF=temp/et-validation/proxy-access-stealoff.conf H3_SCEN=echo \
  python3 temp/h3-timeout-investigation/fixed/repro.py after 150 1 0 --duration 10ms
```

结果位于 `temp/h3-timeout-investigation/fixed/result.log`。该脚本副本每轮先删除旧 JSON，避免读取残留结果。
