# QUIC Stream 退役条件重构 — 实现方案

## Status: Implemented

## Problem Summary

`QuicStream` 当前用一组 `app_released_` 状态来决定一条 stream 何时可以从连接的
`streams_` table 中退役（`retire_stream` → `detach_from_connection`，`conn_` 置空）：

- `retain_app()` / `release_app()` / `mark_app_released()`（`QuicStream.cpp:612-621`）
- 字段 `app_released_` 默认 `true`（`QuicStream.h:201`）
- `ready_for_connection_release()` 要求 `app_released_` 为真（`QuicStream.cpp:623-629`）

问题在于职责不清与死代码：

1. `mark_app_released()` 与 `app_released()` getter 全仓无调用点 —— 纯死代码。
2. `release_app()`（置位 + `try_release_stream`）与 `mark_app_released()`（仅置位）
   两个入口改同一 flag 却副作用不同。
3. 极性反直觉：`app_released_` 默认 `true`（"已释放"），需 `retain_app` 置 `false`。
4. `app_released_` 与 `ref_count_`/`Lease` 是两套并行的"谁占着这条流"追踪，
   必须手工同步 —— 读循环既要持 `Lease` 又要配 `retain_app`/`release_app`。
5. `app_released_` 在服务端双向流上保护的实际是"读循环还在跑"，而非"app 还想写响应"：
   `release_app()` 在 `~RequestScope`（读循环结束）时触发（`ServerHttp3Request.cpp:73`），
   即"请求体读完"就放行退役。一旦 H3 写响应路径落地，会出现"读完请求 → 退役 →
   `conn_=nullptr` → 写不出响应"的隐患。

仓库里 HTTP/2 侧的 `Http2Stream::ready_for_connection_release`
（`Http2Stream.cpp:179-182`）已经用了一套更干净的模型，且无 app flag：

```cpp
return close_reason_ != None || remote_rst_ || local_rst_ ||
       (remote_end_stream_ && local_end_stream_);
```

本方案把 QUIC stream 对齐到这个成熟模型。

## Goals

1. 删除 `app_released_` 及其 4 个方法，消除死代码与双入口歧义。
2. 退役条件改为**方向感知的"本流涉及的每个方向都已结束"**，覆盖双向流与单向流。
3. 保留强制关闭逃生口（`close()`/`reset()` 路径），保证连接 teardown 能清理。
4. 保留"在途数据全部 ack"约束，保证重传路径安全（retire 后无未 ack 数据可丢）。
5. 不改动 `ref_count_`/`Lease` 的现有配对语义（table 持有隐式 ref），范围最小化。

## 非目标

- 不重构 `Lease::adopt` / table 持有 ref 的显式化（现状已平衡，仅清理 flag）。
- 不实现 H3 写响应路径（`ServerHttp3Request::write_body` 仍为 stub）；但本方案为
  其落地铺好退役语义前提。

## 方向语义（覆盖单向流）

| 流类型 | recv 侧 | send 侧 | 退役条件 |
|---|---|---|---|
| Bidi | 有（peer→local） | 有（local→peer） | `recv_done && send_done` |
| Uni, local 发起（local→peer） | N/A | 有 | `send_done` |
| Uni, peer 发起（peer→local） | 有 | N/A | `recv_done` |

强制关闭（`closed_`）作为逃生口，绕过"两端结束"判断。

## 「结束」的精确定义

- `recv_done = recv_queue_.finished() || recv_queue_.reset_received()`
  - `finished()` = `has_final_size_ && next_read_offset_ == received_end_offset_`
    （`QuicStreamRecvQueue.h:55`），即 peer 发了 FIN 且 app 已把缓冲区读空，
    天然包含"app 已消费"。
- `send_done = send_queue_.send_closed() && send_queue_.empty() && !stream_send_pending_`
  - `send_closed()` = `fin_appended_ || reset_sent_`（`QuicStreamSendQueue.h:70`）——
    堵住"app 还没写过任何东西时 `empty()` 误判为 true"的洞
    （`empty()` 含 `!fin_appended_` 分支，`QuicStreamSendQueue.h:66`）。
  - `empty()` 含 `buffered_bytes()==0`（= `ready_bytes_ + inflight_bytes_`），
    保证在途数据全部 ack。

## 实现步骤

### Step 1. `QuicStream` 删除 app flag 机制

**文件**: `src/quic/QuicStream.h`, `src/quic/QuicStream.cpp`

- 删字段 `bool app_released_ = true;`（`QuicStream.h:201`）
- 删方法声明 `retain_app() / release_app() / mark_app_released()`（`QuicStream.h:149-151`）
- 删 getter `app_released()`（`QuicStream.h:123`）
- 删定义 `retain_app() / release_app() / mark_app_released()`（`QuicStream.cpp:612-621`）

### Step 2. `QuicStream` 新增方向标记 `local_initiated_`

**文件**: `src/quic/QuicStream.h`, `src/quic/QuicStream.cpp`

`assign_conn_ctx` 增参 `bool local_initiated`，记录本端是否为发起方（决定 uni 流
适用方向）：

```cpp
// QuicStream.h
bool local_initiated_ = false;
void assign_conn_ctx(QuicConnection &conn, std::uint64_t stream_id,
                     QuicStreamRecvQueue::Options recv_options,
                     bool local_initiated) noexcept;
```

```cpp
// QuicStream.cpp assign_conn_ctx
local_initiated_ = local_initiated;
```

### Step 3. `QuicConnection::attach_stream` 透传方向

**文件**: `src/quic/QuicConnection.h`, `src/quic/QuicConnection.cpp`

`attach_stream`（`QuicConnection.cpp:1076`）是 peer/local 两路唯一汇聚点，加形参
`bool local_initiated` 并透传给 `assign_conn_ctx`：

- peer 路径（`create_peer_stream` / `get_or_create_peer_stream` 调用处）传 `false`
- local 路径（`try_attach_local_stream` 调用处，`:1140`）传 `true`

### Step 4. 重写 `ready_for_connection_release`

**文件**: `src/quic/QuicStream.cpp:623`

```cpp
bool QuicStream::ready_for_connection_release() const noexcept {
    if (!attached_to_connection_) {
        return false;
    }
    // send 侧排空条件（所有路径共用）
    const bool send_drained = send_queue_.empty() && !stream_send_pending_;

    // 强制关闭逃生口：close() 已 reset 发送队列 → empty() 成立。
    // 仍等 pending 帧排空，避免 retire 后 packet number space 残留 STREAM 帧误用。
    if (closed_) {
        return send_drained;
    }

    const bool recv_done = recv_queue_.finished() || recv_queue_.reset_received();
    const bool send_done = send_queue_.send_closed() && send_drained;

    if (bidirectional()) {
        return recv_done && send_done;
    }
    // 单向流：只有适用方向需要结束
    return local_initiated_ ? send_done : recv_done;
}
```

`send_closed()` 复用 `QuicStreamSendQueue` 已有访问器（`QuicStreamSendQueue.h:70`），
无需新增。

### Step 5. `QuicConnection` 删除 app 转发层

**文件**: `src/quic/QuicConnection.h`, `src/quic/QuicConnection.cpp`

- 删声明 `retain_stream_app() / release_stream_app()`（`QuicConnection.h:419-420`）
- 删定义（`QuicConnection.cpp:1541-1543`）

### Step 6. H3 层移除 retain/release_app 调用

**文件**: `src/http/Http3Connection.cpp`, `src/http/ServerHttp3Request.cpp`

- `Http3Connection.cpp:135`：删 `quic_.retain_stream_app(stream);`
- `ServerHttp3Request.cpp`：删 `RequestScope` 及其 `release_app()`（`:70-74, :80`）；
  `run_read_loop` 改为仅持 `Lease` 保证对象存活，退役交给连接在 recv 完结 + send
  完结时统一判定。

  时序：读循环退出仅代表 recv 侧结束；bidi 流要等 app 写完响应（`write_body` 发 FIN
  → `fin_appended_=true`）且全部 ack 后，连接的 `on_stream_send_acked` →
  `try_release_stream` 才真正 retire。

### Step 7. 关联队列清理

**文件**: `src/quic/QuicStream.cpp` (`detach_from_connection:657`)

现状已置 `conn_=nullptr; attached_to_connection_=false; stream_send_pending_=false;
stream_data_blocked_reported_=false`。补充兜底：detach 时显式 `send_queue_.reset()`
（若尚未 closed），保证退役瞬间发送侧缓冲释放，无悬挂。

审计项（非阻塞）：packet number space 的 `pending_frames` 中可能残留
`type=Stream` 的 `QuicOutputFrame`。编码路径 `encode_stream_frame` 首查
`attached_to_connection_`（`QuicStream.cpp:561`），detach 后 `Skipped` 不误用，
条目随正常排空释放。在验证阶段构造场景确认无 UAF、无悬空 stream_id 编码。

## 调用链影响审计

`try_release_stream` 现有触发点在新模型下仍正确：

| 触发点 | 行 | 新模型行为 |
|---|---|---|
| `on_stream_data_recv` 后 | `:1403` | peer 发 FIN 后 `recv_done` 成立，但 bidi 还需 `send_done` → 不误 retire；peer-uni 若此时 `finished()`（如 length-0 FIN 或 app 已消费完数据后续到的 FIN）则 retire |
| `on_remote_reset` 后 | `:1460` | `reset_received` → recv_done；bidi 仍等 send |
| `on_stream_send_acked` | `:2333` | 响应 FIN ack 后 `send_done` 成立 → 配合 recv_done retire（bidi 关键正确路径） |
| `drop_stream_send_ticket` | `:2258` | `stream_send_pending_=false` 后重判 |
| `QuicStream::close()` | `:515` | `closed_` 逃生口；连接 teardown 经 `close_all_streams` 走此路径 |
| `on_stream_send_failed` | — | 重传排队，不 retire |

### 触发器设计决策（实现期澄清）

**不在 `try_read`/`read` 中触发 `try_release_stream`。** 实现期曾尝试在 app
消费完接收侧数据后自动触发退役，但这会破坏 H3 单向流读循环：peer-uni 控制流的类型
字节与 FIN 同帧到达，app 第一次 `read` 消费完所有数据即 `finished()` → 触发退役 →
`detach_from_connection` → `conn_=nullptr`，读循环下一次 `read` 返回 `Invalid`（而非
期望的 `0 + complete`），导致 `FrameError` 而非 `ClosedCriticalStream`。

结论：保留旧模型的触发集（去掉 `release_app`），现有触发点已覆盖所有现实路径：
- bidi 请求流：响应 FIN 全部 ack → `on_stream_send_acked` 触发（关键路径）。
- peer-uni 流：FIN 帧到达时若 `finished()` 即退役；数据+FIN 同帧时 `finished()` 在
  recv 时尚不成立（app 未消费），故在连接关闭（`close_all_streams`）时退役 —— 与旧
  模型一致。
- 强制关闭：`close()` 逃生口。

重传安全：`on_stream_send_acked/Failed`、`should_retransmit_stream_data_blocked` 均
`find_stream`，retire 后返回 null → no-op。由于 retire 要求 `send_queue_.empty()`
（in-flight=0），retire 时无未 ack 数据，不会漏重传。

## 行为变化与风险

1. **正向变化**：bidi 请求流在读循环结束后、写响应前不再被提前退役 —— 补上旧
   `app_released_` 在 H3 写路径落地时的隐患。
2. **可接受的泄漏语义**：app 读完请求但从不发响应+FIN 的 bidi 流，会留存到连接关闭
   （`close_all_streams` → `closed_` 逃生）。与 H2 一致；不发响应的 server 本就是 bug。
3. **H3 写响应路径前置依赖**：`ServerHttp3Request::write_body`（当前 stub）落地时，
   必须在发出 FIN/RESET 后由连接侧 `on_stream_send_acked` 触发退役。写路径实现须遵守。
4. **uni 流无回归**：peer uni 流（control/QPACK）当前 `app_released_` 默认 true 即按
   recv 完结退役，新模型 `recv_done` 等价；local uni 流当前实际只能靠 `closed_` 退役
   （recv 永不 finished），新模型 `send_done` 修复了这一点，为后续 H3 本端控制流铺路。
5. **ref_count 配对不变**：table 持有的隐式 ref（insert 的 `release_raw` / erase 的
   `adopt`）语义不变，无需改动 `Lease`。

## 改动文件清单

| 文件 | 改动 |
|---|---|
| `src/quic/QuicStream.h` | 删 `app_released_` 字段及 4 个方法声明；加 `local_initiated_` 字段；`assign_conn_ctx` 加参 |
| `src/quic/QuicStream.cpp` | 删 3 个 app 方法定义；`assign_conn_ctx` 加参设值；重写 `ready_for_connection_release`；`detach_from_connection` 补 `send_queue_.reset()` 兜底 |
| `src/quic/QuicConnection.h` | 删 `retain_stream_app/release_stream_app` 声明；`attach_stream` 加 `bool local_initiated` 参 |
| `src/quic/QuicConnection.cpp` | 删 2 个 app 转发定义；`attach_stream` 透传；peer/local 两路调用传 `false`/`true` |
| `src/http/Http3Connection.cpp` | 删 `:135` 的 `retain_stream_app` 调用 |
| `src/http/ServerHttp3Request.cpp` | 删 `RequestScope` 及 `release_app`；`run_read_loop` 仅持 Lease |

## 验证计划

1. **单元测试**：`QuicStream` 退役条件矩阵 —— bidi（recv-only done / send-only done /
   都 done）、peer-uni（recv done）、local-uni（send done）、`closed_` 逃生口；每类
   断言 `ready_for_connection_release` 真值。
2. **回归**：现有 QUIC/H3 测试全绿（`ctest --test-dir build`），重点跑 stream 生命周期、
   FIN、reset、graceful close 用例。
3. **关联队列审计**：构造"stream 有待打包 STREAM 帧时被 close/retire"场景，确认无 UAF、
   无悬空 stream_id 编码。
