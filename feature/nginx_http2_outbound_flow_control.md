# Nginx HTTP/2 Outbound 排队与发送窗口处理

## 文档范围

本文记录仓库固定版本 Nginx 1.31.3 的 HTTP/2 outbound 行为，重点覆盖：

- DATA frame 创建前的 connection/stream 两级窗口检查；
- frame header 的内存分配与编码时机；
- frame 排队和发送窗口实际扣减的顺序；
- `EAGAIN`、partial write、stream cleanup 和连接失败时的窗口处理；
- `WINDOW_UPDATE` 与 `SETTINGS_INITIAL_WINDOW_SIZE` 如何恢复发送。

这里描述的是 Nginx 实现，不替代 HTTP/2 协议规范。Nginx 源码来自：

- 版本定义：`scripts/build_nginx.sh`，`nginx_version="1.31.3"`；
- HTTP/2 filter：`temp/nginx-1.31.3/src/http/v2/ngx_http_v2_filter_module.c`；
- HTTP/2 connection：`temp/nginx-1.31.3/src/http/v2/ngx_http_v2.c`；
- HTTP/2 类型和编码宏：`temp/nginx-1.31.3/src/http/v2/ngx_http_v2.h`。

## 核心结论

Nginx 对 outbound DATA 使用“队列预留窗口”的记账方式：

```text
读取两级 send window 并计算本轮预算，不修改窗口
    -> 按预算切分 payload
    -> 分配或复用 9-byte frame header 内存
    -> 编码 frame header
    -> 将 frame 挂入 connection output queue
    -> 同时扣减 connection 和 stream send window
    -> 尝试写入真实 TCP/TLS connection
```

因此：

- 不是先修改窗口再尝试创建 frame；
- 不是等 socket 写成功后才扣窗口；
- frame 已经排队时，其 DATA payload 已经占用两级窗口额度；
- `EAGAIN` 和 partial write 不退窗口，因为 frame 仍会继续发送；
- stream cleanup 删除尚可取消的 DATA frame 时，只恢复 connection window；
- 已经成为 output queue 阻塞点的 frame 不会被 cleanup 删除，也不退窗口。

## 两类 connection

Nginx HTTP/2 输出路径中存在两种 connection：

- `fc = r->connection`：每个 HTTP/2 stream 对应的 fake connection；
- `h2c->connection`：承载所有 streams 的真实 TCP/TLS connection。

stream 初始化时，Nginx 将 fake connection 的 `send_chain` 替换为 HTTP/2 实现：

```c
fc->send_chain = ngx_http_v2_send_chain;
```

位置：`ngx_http_v2_filter_module.c:811-838`。

普通 HTTP write filter 调用：

```c
chain = c->send_chain(c, r->out, limit);
```

位置：`src/http/ngx_http_write_filter_module.c:294-304`。对于 HTTP/2 stream，该调用先进入 `ngx_http_v2_send_chain()` 完成 framing 和排队；真正发送时才调用真实 connection 的 `c->send_chain()`。

## DATA 创建前的窗口检查

主体入口是 `ngx_http_v2_send_chain()`：

- 位置：`ngx_http_v2_filter_module.c:1063-1284`。

对于非空 DATA，Nginx 在创建 frame 前调用 `ngx_http_v2_flow_control()`：

```c
if (stream->send_window <= 0) {
    stream->exhausted = 1;
    return NGX_DECLINED;
}

if (h2c->send_window == 0) {
    ngx_http_v2_waiting_queue(h2c, stream);
    return NGX_DECLINED;
}
```

位置：`ngx_http_v2_filter_module.c:1408-1427`。

如果窗口不足，Nginx 会先调用 `ngx_http_v2_filter_send()`，尝试清空已经存在的 output queue，然后再次检查窗口。第二次检查仍失败时：

```c
fc->write->active = 1;
fc->write->ready = 0;
return in;
```

位置：`ngx_http_v2_filter_module.c:1111-1121`。

此时新的 DATA frame 尚未创建：

- 没有分配或编码新的 frame header；
- 没有把新的 frame 加入队列；
- 没有扣减新的窗口额度；
- 原始 input chain 被返回，等待之后继续处理。

## 发送预算计算

窗口检查通过后，Nginx 计算本轮最多能够变成 DATA frame 的 payload 字节数：

```c
if (limit == 0 || limit > (off_t) h2c->send_window) {
    limit = h2c->send_window;
}

if (limit > stream->send_window) {
    limit = (stream->send_window > 0) ? stream->send_window : 0;
}
```

位置：`ngx_http_v2_filter_module.c:1144-1150`。

可以概括为：

```text
budget = min(
    write_filter_limit，0 表示不额外限制,
    connection_send_window,
    max(stream_send_window, 0)
)
```

这一步只是读取窗口和计算局部变量 `limit`，尚未修改 `h2c->send_window` 或 `stream->send_window`。

单个 DATA frame 的 payload 上限首先取：

```c
frame_size = min(h2lcf->chunk_size, h2c->frame_size);
```

位置：`ngx_http_v2_filter_module.c:1152-1155`。其中 `http2_chunk_size` 默认 8 KiB，`h2c->frame_size` 是对端允许的最大 frame size，初始为 16 KiB，并可被对端 `SETTINGS_MAX_FRAME_SIZE` 更新。

随后每个 frame 再受剩余预算限制：

```c
if ((off_t) frame_size > limit) {
    frame_size = (size_t) limit;
}
```

位置：`ngx_http_v2_filter_module.c:1163-1166`。

例如：

```text
待发送 body              = 32 KiB
connection send window   = 10 KiB
stream send window       = 4 KiB
http2_chunk_size         = 8 KiB

本次实际创建 DATA payload = 4 KiB
剩余 28 KiB input         = 保留，等待 WINDOW_UPDATE
```

## Frame header 内存和编码时机

`ngx_http_v2_filter_get_data_frame()` 在 frame 排队前分配或复用 header buffer：

- 位置：`ngx_http_v2_filter_module.c:1320-1405`。

header 使用独立的 9-byte `ngx_buf_t`：

```c
cl = ngx_chain_get_free_buf(stream->request->pool,
                            &stream->free_frame_headers);

buf = cl->buf;

if (buf->start == NULL) {
    buf->start = ngx_palloc(stream->request->pool,
                            NGX_HTTP_V2_FRAME_HEADER_SIZE);
    buf->end = buf->start + NGX_HTTP_V2_FRAME_HEADER_SIZE;
    buf->tag = (ngx_buf_tag_t) &ngx_http_v2_module;
    buf->memory = 1;
}
```

复用时重置位置，然后立即编码完整 frame header：

```c
buf->pos = buf->start;
buf->last = buf->pos;

buf->last = ngx_http_v2_write_len_and_type(buf->last, len,
                                           NGX_HTTP_V2_DATA_FRAME);
*buf->last++ = flags;
buf->last = ngx_http_v2_write_sid(buf->last, stream->node->id);
```

9 字节布局为：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 3 | payload length，24-bit，大端序 |
| 3 | 1 | frame type |
| 4 | 1 | flags |
| 5 | 4 | reserved bit + stream ID，大端序 |

`ngx_http_v2_write_len_and_type()` 将 24-bit length 和 8-bit type 合并成一次 32-bit 网络序写入；定义位于 `ngx_http_v2.h:339-368`。

编码后，header chain link 被放到 payload chain 前面：

```text
frame->first
    -> [9-byte encoded header ngx_buf]
    -> [payload ngx_buf / shadow ngx_buf]
    -> [payload ngx_buf / shadow ngx_buf]
    -> frame->last
```

payload 切片使用 shadow buffer 调整 `pos/last` 或 `file_pos/file_last`，避免复制原始 body 数据。位置：`ngx_http_v2_filter_module.c:1287-1317`。

## 编码、排队和扣减的准确顺序

在 `ngx_http_v2_send_chain()` 中，顺序明确为：

```c
frame = ngx_http_v2_filter_get_data_frame(stream, frame_size,
                                          out, cl);
if (frame == NULL) {
    return NGX_CHAIN_ERROR;
}

ngx_http_v2_queue_frame(h2c, frame);

h2c->send_window -= frame_size;
stream->send_window -= frame_size;
stream->queued++;
```

位置：`ngx_http_v2_filter_module.c:1232-1245`。

所以各步骤语义如下：

1. `ngx_http_v2_filter_get_data_frame()` 成功后，header 已完成编码；
2. `ngx_http_v2_queue_frame()` 将 frame 挂入 `h2c->last_out`；
3. queue 操作本身没有失败返回值；
4. frame 已在队列后才扣减两级窗口；
5. `stream->queued` 记录该 stream 尚未完成的 frame 数量。

如果 frame 对象或 header buffer 分配失败，函数在排队和扣窗口之前返回 `NGX_CHAIN_ERROR`，因此不存在需要回滚的窗口修改。

## 窗口只计算 DATA payload

扣减值是 `frame_size`，即 DATA payload 长度：

- 9-byte frame header 不占 connection 或 stream flow-control window；
- HEADERS、CONTINUATION、SETTINGS、PING、RST_STREAM 等不扣 DATA flow-control window；
- 零长度 DATA 不扣窗口；
- 零长度 `DATA + END_STREAM` 可以在窗口为 0 时创建和发送。

代码只在 `size != 0` 时执行前置 flow-control 拒绝逻辑，而 `last_buf` 仍可触发零长度 DATA frame 创建，位置：`ngx_http_v2_filter_module.c:1084-1122` 和 `1232-1245`。

## Connection output queue

普通 DATA frame 使用 `ngx_http_v2_queue_frame()` 排入 connection 公共队列：

- 位置：`ngx_http_v2.h:244-267`。

该函数依据 stream dependency rank 和 relative weight 选择插入位置。HEADERS 和部分控制帧使用 blocked/ordered queue 保证输出顺序。这里的 `blocked` 是 output queue 的排序/不可穿越状态，不等同于 flow-control window 耗尽。

`ngx_http_v2_filter_send()` 调用 `ngx_http_v2_send_output_queue()` 尝试发送公共队列：

- 位置：`ngx_http_v2_filter_module.c:1462-1492`。

如果该 stream 仍有 queued frame，fake connection 会设置 `NGX_HTTP_V2_BUFFERED` 并返回 `NGX_AGAIN`。

## 实际写入和 partial write

`ngx_http_v2_send_output_queue()` 将所有 frame 的 `first..last` chain 串成一个发送 chain，然后调用真实 connection：

```c
for (frame = h2c->last_out; frame; frame = fn) {
    frame->last->next = cl;
    cl = frame->first;
    /* ... */
}

cl = c->send_chain(c, cl, 0);
```

位置：`ngx_http_v2.c:509-550`。

发送前不会再次编码 frame header。底层发送函数直接消费已经编码好的 header buffer 和 payload buffers：

- Linux 明文连接通常走 `ngx_linux_sendfile_chain()`；
- 内存 buffers 通过 `writev()`；
- 文件 payload 可通过 `sendfile()`；
- TLS 连接走 `ngx_ssl_send_chain()`，必要时把已经编码好的 HTTP/2 字节复制到 SSL 聚合 buffer，再调用 `SSL_write()`。

底层通过推进 `ngx_buf_t::pos` 或 `file_pos` 表示已消费字节。之后每个 frame 的 handler 检查是否完整发送。

DATA handler 位置：`ngx_http_v2_filter_module.c:1551-1643`。

## 各种结果下的窗口处理

### 1. 窗口完全不足

条件：

- `stream->send_window <= 0`；或
- `h2c->send_window == 0`。

处理：

- 不创建新的非空 DATA frame；
- 不编码新的 DATA frame header；
- 不排队；
- 不扣窗口；
- stream 等待 `WINDOW_UPDATE` 或 SETTINGS window delta。

### 2. 窗口只够部分 payload

处理：

- 只为窗口覆盖的 payload 创建 DATA frame；
- header 中的 payload length 写实际 frame payload 长度；
- frame 排队后按实际 payload 长度同时扣减 connection/stream window；
- 未覆盖的 input 保留到之后继续 framing。

### 3. Frame 创建失败

可能原因包括 frame/header buffer 分配失败或 flood 限制。处理：

- 返回 `NGX_CHAIN_ERROR`；
- frame 未排队；
- 两级窗口尚未扣减；
- 不需要窗口回滚。

### 4. Frame 已排队并一次发送完成

处理：

- 窗口保持已经扣减的状态；
- frame handler 回收 chain/header/frame；
- `stream->queued--`；
- 如果带 `END_STREAM`，设置 `stream->out_closed = 1`。

frame 完成处理位置：`ngx_http_v2_filter_module.c:1646-1669`。

窗口不会在发送完成时恢复。只有收到对端 `WINDOW_UPDATE` 才表示新的发送额度。

### 5. Socket 返回 EAGAIN，未写出或未写完

处理：

- frame 保留在 `h2c->last_out`；
- 已扣 connection window 不恢复；
- 已扣 stream window 不恢复；
- write event 变为 not ready；
- 注册发送超时并等待下一次 write event；
- 下一次从当前 `buf->pos/file_pos` 继续发送，不重新编码、不再次扣窗口。

`ngx_http_v2_send_output_queue()` 的重排和 `NGX_AGAIN` 处理位于 `ngx_http_v2.c:576-603`。

### 6. Frame header partial write

如果 9-byte header 只发送了一部分：

- header buffer 的 `pos` 停在未发送字节；
- DATA handler 返回 `NGX_AGAIN`；
- frame 被标记为 `blocked`；
- 下次直接从 header 当前 `pos` 继续；
- 两级窗口均不恢复。

位置：`ngx_http_v2_filter_module.c:1562-1581`。

### 7. Header 已完成但 payload partial write

处理：

- 已消费的 header chain link 可以进入 `stream->free_frame_headers`；
- shadow payload 的进度同步回原始 buffer；
- `frame->first` 移到尚未完成的 payload chain link；
- frame handler 返回 `NGX_AGAIN`；
- 下次从剩余 payload 继续；
- 两级窗口均不恢复。

位置：`ngx_http_v2_filter_module.c:1584-1626`。

### 8. Stream cleanup 取消尚可移除的 DATA frame

stream cleanup 遍历 connection output queue，移除属于该 stream 且 `!frame->blocked` 的 frame：

```c
if (frame->stream == stream && !frame->blocked) {
    *fn = frame->next;
    window += frame->length;

    if (--stream->queued == 0) {
        break;
    }

    continue;
}
```

位置：`ngx_http_v2_filter_module.c:1703-1746`。

被取消 frame 的 `frame->length` 累加到局部变量 `window`，最后只恢复 connection window：

```c
h2c->send_window += window;
```

位置：`ngx_http_v2_filter_module.c:1748-1770`。

不会恢复 `stream->send_window`，因为该 stream 正在关闭，stream-level 额度不再供任何后续发送使用；connection-level 额度则必须归还，才能供其他 streams 使用。

如果恢复前 connection window 为 0，cleanup 还会唤醒 connection waiting queue 中的 streams。

### 9. 已标记 blocked 的 frame 遇到 stream cleanup

frame handler 返回非 `NGX_OK` 时，output queue 将 frame 标记为：

```c
out->blocked = 1;
```

位置：`ngx_http_v2.c:576-582`。

注意，`blocked` 不严格等于“socket 已经写出至少一个字节”：底层直接返回 `EAGAIN` 时，frame 也可能被标记为 blocked。它表示该 frame 已成为 connection 输出序列中不可直接取消或穿越的位置。

stream cleanup 只删除 `!frame->blocked` 的 frame，因此 blocked frame：

- 不从 output queue 删除；
- 不恢复 connection window；
- 不恢复 stream window；
- 保留在 connection 输出链上继续处理。

### 10. 整个 connection 发送失败

真实 connection 的 `send_chain()` 返回 `NGX_CHAIN_ERROR` 时：

- `c->error = 1`；
- write event 被投递以完成 connection teardown；
- 不执行正常的窗口回退事务。

位置：`ngx_http_v2.c:546-550` 和 `611-621`。

整个 HTTP/2 connection 即将关闭时，connection/stream window 已不再用于后续调度，因此恢复窗口没有实际意义。

## WINDOW_UPDATE 恢复发送额度

收到 `WINDOW_UPDATE` 时：

- `sid != 0`：只增加对应 `stream->send_window`；
- `sid == 0`：只增加 `h2c->send_window`。

stream WINDOW_UPDATE 路径位于 `ngx_http_v2.c:2441-2488`：

```c
stream->send_window += window;
```

如果 stream 曾标记为 `exhausted`，Nginx 清除此标记并触发 fake connection 的 write handler。

connection WINDOW_UPDATE 路径位于 `ngx_http_v2.c:2491-2527`：

```c
h2c->send_window += window;
```

随后 Nginx 按 waiting queue 顺序唤醒 streams；如果新增加的 connection window 又被前面的 streams 消耗为 0，则停止继续唤醒。

## SETTINGS_INITIAL_WINDOW_SIZE

新 HTTP/2 connection 的 outbound 窗口初值：

```c
h2c->send_window = NGX_HTTP_V2_DEFAULT_WINDOW;  /* 65535 */
h2c->init_window = NGX_HTTP_V2_DEFAULT_WINDOW;  /* 65535 */
```

位置：`ngx_http_v2.c:242-247`。

新 stream 初始化：

```c
stream->send_window = h2c->init_window;
```

位置：`ngx_http_v2.c:3111`。

对端修改 `SETTINGS_INITIAL_WINDOW_SIZE` 时，Nginx 计算 delta，并将其应用到所有活跃 streams：

```c
stream->send_window += delta;
```

位置：`ngx_http_v2.c:4904-4961`。

因此 stream send window 使用有符号 `ssize_t`：SETTINGS 减小 initial window 后，已经发送/预留较多 DATA 的 stream window 可能变成负数。connection send window 不受 `SETTINGS_INITIAL_WINDOW_SIZE` 影响，只能由 connection-level `WINDOW_UPDATE` 增加。

## 状态汇总

| 情况 | 是否编码新 DATA header | 是否入队 | Connection window | Stream window | 后续动作 |
|---|---:|---:|---|---|---|
| 两级窗口均可用 | 是 | 是 | 入队后扣 payload | 入队后扣 payload | 立即尝试发送 |
| 只有部分窗口 | 是，length 为可用部分 | 是 | 扣实际 payload | 扣实际 payload | 剩余 input 等待窗口 |
| connection window 为 0 | 否 | 否 | 不变 | 不变 | 进入 connection waiting queue |
| stream window 小于等于 0 | 否 | 否 | 不变 | 不变 | 标记 stream exhausted |
| frame/header 分配失败 | 可能未完成 | 否 | 不变 | 不变 | 返回错误 |
| 已入队后 EAGAIN | 已编码 | 保留 | 不恢复 | 不恢复 | 等待 write event |
| partial header/payload | 已编码 | 保留并标记 blocked | 不恢复 | 不恢复 | 从当前 buffer 位置续写 |
| cleanup 删除 `!blocked` DATA | 已编码但取消 | 删除 | 加回 frame payload | 不加回 | 额度给其他 streams |
| cleanup 遇到 blocked DATA | 已编码 | 保留 | 不恢复 | 不恢复 | connection 继续处理该 frame |
| DATA 完整发送 | 已编码 | 完成后回收 | 不恢复 | 不恢复 | 等待对端 WINDOW_UPDATE |
| 零长度 DATA + END_STREAM | 是 | 是 | 不扣 | 不扣 | 关闭 outbound stream |
| HEADERS/控制帧 | 是 | 是 | 不受 DATA 窗口影响 | 不受 DATA 窗口影响 | 仍受 socket backpressure |
| connection 发送错误 | 已编码 | connection teardown | 不回退 | 不回退 | 关闭 connection |

## 对本仓库实现的设计约束

以下是从 Nginx 行为提取的实现建议，不表示当前 `fiber-gateway-cpp` 已经完全采用相同策略：

1. 将“预算计算”和“窗口状态修改”分开。预算计算可以失败或返回 0，不应提前修改窗口。
2. 只有 frame header、payload 引用和队列节点都准备成功后，才把 frame 发布到 connection queue 并提交两级窗口扣减。
3. 队列发布和窗口扣减应位于同一个 event-loop 临界区，不能让其他 stream 观察到“已排队但未扣窗口”或“已扣窗口但未排队”的中间状态。
4. 把 queued DATA 的窗口额度视为 reservation。socket `EAGAIN` 或 partial write 只改变发送游标，不改变 reservation。
5. 取消尚未进入不可中断输出位置的 DATA 时，必须把 connection reservation 退给其他 streams；stream 已销毁时无需恢复 stream window。
6. 如果实现允许取消一个已部分发送的 frame，必须精确区分已发送 payload 和未发送 payload，不能简单按整个 frame length 回退；Nginx 选择保留 blocked frame，从而避免这种复杂回滚。
7. HEADERS/control frame 与 DATA flow control 分离，但仍需共享 connection socket backpressure 和严格的 frame 输出顺序。
8. 零长度 END_STREAM DATA 应绕过 DATA payload 窗口检查，否则可能在双方都等待窗口时无法正常结束 stream。
