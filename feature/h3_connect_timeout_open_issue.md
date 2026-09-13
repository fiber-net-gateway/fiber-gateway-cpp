# 未解决：H3 压测启动阶段偶发单连接握手超时

日期：2026-09-13。状态：**未归因**。分支 `feat/epoll-et-scheduling`（`bcb57b4` 之后）。

本文只记录事实与已排除项，供后续排查。相关背景见 `feature/epoll_et_design.md` §14.7（已修复的 keep-alive 伪唤醒问题）。

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
