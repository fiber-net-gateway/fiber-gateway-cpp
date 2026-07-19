# `src/http/` 性能优化审计

> 审计范围：`src/http/` 全部 117 个文件（约 2.68 万行），按 HTTP/1、HTTP/2 core、HPACK/Huffman、HTTP/3+QPACK、shared/common 五个域并行评审后整合。所有结论均经直接读码 + 抽样核验。
>
> 核验事实：`IoBuf::allocate` 是裸 `::operator new`（无池，见 `src/common/mem/IoBuf.cpp`）；`HttpHeaders` 仅有 `get/contains(string_view)` 无 hash 重载（`HttpHeaders.h:57-58`）；HPACK/QPACK 两个编码器都有同一 Huffman-size bug（`encoded_len` 算了却从不和 `value.size()` 比较）。
>
> 状态：持续推进中；标记为 ✅ 已修复的条目已经落地，其余按下方建议顺序继续优化。

---

## 整体印象

代码与项目性能优先约定契合度很高：HPACK/Huffman 两级表是标准高性能设计、连接池走分片+无锁偷取、IoBuf 零拷贝贯穿读写路径、协程 awaiter 栈上分配。问题集中在若干 **高频路径上的冗余分配/拷贝/系统调用** 和 **两处跨协议重复的根因**。

---

## 一、最高优先级（热路径，每次请求都付代价）

### 1. HTTP/1 chunked `write_body` 每个 chunk 4 次 syscall ✅ 已修复
- **位置**：`Http1ExchangeIo.cpp:982-994`（server）+ `ClientHttp1Exchange.cpp:807-824`（client，3 次）
- **问题**：每个 chunk 独立 `co_await write_all(size)`、`write_all("\r\n")`、`write_all(chunk)`、`write_all("\r\n")`，结尾 `"0\r\n\r\n"` 还是单独一次（`:1004`）。而 `write_all(HttpTransport*, IoBufChain&)` 重载（`:178`）已存在——client 自己甚至已把 size+CRLF 合进一个前缀。注意 `:1070-1092` 是第二个 chunked 入口，同样 4 次。
- **场景**：流式/SSE/代理响应每个 chunk ≥4 syscall 而非 1。
- **修法**：把 `size+CRLF+data+CRLF` 拼成一个 `IoBufChain` 走一次 writev。

### 2. QPACK 静态表 O(99) 线性扫描，且伪头走通用 `find()` ✅ 已修复
- **位置**：`Http3QpackStaticTable.cpp:137-168`（`find()`）；`Http3QpackEncoder.cpp:90-117,132-139`（伪头编码）
- **问题**：每个出站 header 全表扫 99 条。`:path /api/users` 命中名字但不命中值时仍扫完 99 条确认无精确值匹配。`encode_status/encode_method/encode_scheme/encode_path` 本可直接用静态下标（`:status 200`=25、`:method GET`=17、`:scheme https`=23、`:path /`=1），却全走 `encode_field->find()`。worst case `:path` 非 `/` = 99 次迭代；`:status 500` = 92 次。
- **修法**：按 `name_hash` 建桶 / 最小完美哈希；伪头用编译期 switch 直接 `append_indexed`，仅对无静态项的值回退 `find()`。
- **落地（2026-07-12）**：方案 a+b 同时实现。`Http3QpackStaticTable::find()` 改为 separate-chaining hash 桶（`kBucketCap=256`，进程级 magic static 一次构建），仅遍历同名桶——`find()` 现最坏链长 `:status` 14 条（原 99）。五个伪头编码器抽 `dispatch(name,value,FindResult)` 共享派发，改用 RFC 9204 编译期 index 直查（`resolve_status/method/scheme/path/authority`）：in-table 值直接 `append_indexed`，否则 `append_literal_static_name` 回退首名下标（`status`=24/`method`=15/`scheme`=22/`path`=1/`authority`=0）。字节输出与原路径逐位等价。新增 7 例测试（500/502/POST/MKCOL/https/http/`/`），全量 1099 ctest 绿。

### 3. Huffman 编码即使让字符串变长也照发（HPACK + QPACK 同一 bug）✅ 已修复
- **位置**：`Http2HpackEncoder.cpp:445`（`should_huffman_encode` 只判 `size >= huffman_threshold`）+ `Http2HpackEncoder.cpp:332-334`（`encoded_len` 只和 `max_string_size` 比）；`Http3QpackEncoder.cpp:330` + `:210-212` 同样
- **问题**：`encoded_len` 算出来了却 **从不和 `value.size()` 比较**。大写为主、random token、base64、已压缩内容会被膨胀最多 5/4×，既费 CPU 又费带宽。RFC 7541 §6.2 本就建议 Huffman 仅在更小时用。
- **修法**：算完 `encoded_len` 后 `if (encoded_len >= value.size()) 走 raw`。零成本，两个文件一起改。

### 4. HTTP/2 读缓冲在背压下每读周期 64KB malloc+memcpy
- **位置**：`Http2Connection.cpp:92-125`（`prepare_read_buffer`），由 `:477` 的 `retain_slice` 触发
- **问题**：body 切片入队后 `read_buf` 不再 `unique()`，下一轮读到数据就重新分配 64KB 并 memcpy 所有未读字节。背压 / 单读多帧场景下持续 churn，且每个未消费切片钉住 64KB。`unread == 0` 但 `!unique()` 时当前代码仍分配新缓冲。
- **修法**：小 DATA 帧直接拷出字节让 `read_buf` 立刻回到 `unique()`；或维护小读缓冲池而非每次 `IoBuf::allocate`；至少 `unread==0` 时复用缓存缓冲而非 `allocate`。

### 5. H3 QPACK 解码器 scratch 每请求 malloc
- **位置**：`Http3QpackDecoder.cpp:75`（`scratch_` 为 `unique_ptr<uint8_t[]>`），`HeaderBlockParser` 在 `ServerHttp3Request.cpp:904` 按 header block 栈上构造；`init()`（`:17`）不预分配，首次字面量时 `new[]`（`:341-358`），析构时 `delete[]`
- **问题**：每个含字面量（user-agent / cookie / 自定义头 / 非 `/` 路径——即常见情况）的请求头块 `new[]/delete[]`，超容量时还有第二次 `new[]+memcpy+delete[]`。绕过项目 BufPool 纪律。
- **修法**：让 `Http3QpackDecoder` 成为 `ServerHttp3Request` 成员跨请求复用；或 scratch 改挂请求级 `BufPool`；至少 `init()` 预分配小 scratch（如 256B）按需增长。

---

## 二、中优先级（根因类，修一处惠及多处）

### 6. `HttpHeaders` 缺 `get(name,hash)` / `contains(name,hash)` 重载 ⭐ 根因 ✅ 已修复
- **位置**：`HttpHeaders.h:57-58`（只有 `get/contains(string_view)`）；`HttpHeaders.cpp:203`（`get_all(string_view)` 额外堆分配 `std::string owned_key_`）
- **问题**：每次调用现算 `http_header_name_hash` 并逐字节大小写折叠。调用点遍布各协议热路径：`GrpcClient` 的 `grpc-status`、`GrpcStream` 的 `grpc-message`、`HttpClientFuncs` 的 `content-type`、`Http1ExchangeIo.cpp:705-709` 每响应查 `Content-Length/Transfer-Encoding/Connection` 三个字面量。代码库已有 `constexpr` 哈希习惯（`HttpProxyCore.h:97`、`ProxyHandler.cpp:129`、`Http2HpackEncoder.cpp:21-25`），但 `get/contains` 没开放重载去吃它。`get_all(string_view)` 还每请求 malloc/free（`ScriptExchangeCtx.cpp:141/202` 的 `get_all("cookie")`）。
- **修法**：补 `get/contains(name,hash)` 与 `get_all(string_view)` 内部走 hash 路径（`MatchRange.owned_key_` 在 pre-hashed 路径下成死字段）；调用点用 `static constexpr uint64_t kXHash = http_header_name_hash("...")`。

### 7. `HttpExchange` 不暴露已解析的 content-length / chunked ⭐ 根因 ✅ 已修复
- **位置**：`HttpExchange.h:140-142`（`request_chunked_/request_content_length_*` 私有无 accessor）；`HttpProxyCore.h:152-169`（`detect_request_body`）
- **问题**：H1/H3 请求头处理时早已解析过（`Http1Connection.cpp:128-149`、`ServerHttp3Request.cpp:773-777`），但 `detect_request_body` 每个代理请求重新 `get("content-length")` + `contains("transfer-encoding")`——重复 2 次哈希+扫描。
- **修法**：加 `request_chunked()` / `bool request_content_length_set()` / `size_t request_content_length()` public accessor，`detect_request_body` 直接读。零每请求开销。

### 8. HPACK `resolve_name_index` 冗余 O(61) 扫描 + catalog 重复查找 ✅ 已修复
- **原问题**：catalog 已有全部 61 条静态项的哈希表，旧实现却在未命中索引表示时再线性扫描静态表，并在 `encode_field` 内重复查 catalog。
- **当前实现**：删除运行期分配的 `Http2HpackEncodeCatalog`，由 `Http2HpackStaticTable` 内的 128 槽编译期开放寻址索引直接返回 `name_index` / `exact_index`；52 个固定名称的已知命中平均探测 1.12 次、最多 2 次，无堆分配、magic-static 初始化、静态表扫描或元数据复制。

### 9. H3 索引静态表字段无谓拷贝到 pool ✅ 已修复
- **位置**：`ServerHttp3Request.cpp:479-481,657-669`（`on_indexed_field` -> `commit_field` -> `commit_regular_header` 把 name/value `copy_to_pool`）
- **问题**：`entry.name/value` 指向 `kEntries_` 字面量（永久存储、已小写），而 `HttpHeaders::add_view`（`HttpHeaders.cpp:153-174`）本就是零拷贝。EntryMatch 是 QPACK 快路径（`accept-encoding: gzip,deflate,br`、`:method GET` 等很常见），却都做 2 次无谓 memcpy 进 arena。
- **修法**：静态存储字段直接 `add_view` 传指针（`lowcase_name = entry.name`），跳过 `copy_to_pool`。用 `name_owned`/`value_owned` 标志或"指向静态存储"谓词门控。

### 10. H1 头解析缓冲每请求 malloc 8KB
- **位置**：`Http1Connection.cpp:194`（`Http1HeaderParseBuffer` 在 `parse_request` 内构造，`ensure_init` 裸 `IoBuf::allocate(8KB)`）
- **问题**：keep-alive 连接 N 请求 = N×8KB malloc/free（外加解析缓冲增长的 realloc）。连接在 `run()` 循环（`:372-417`）里跨请求存活，缓冲本可复用。`Http1HeaderParseBuffer::reset()` 已存在。
- **修法**：把 `Http1HeaderParseBuffer` 提为 `Http1Connection` 成员，每请求 `reset()`/`ensure_init()` 而非重构。

---

## 三、低优先级 / 卫生

| 位置 | 问题 | 修法 |
|------|------|------|
| `Http2Connection.cpp` | ✅ 已修复：控制帧与 stream hook 先汇总到稳定的 in-flight chain，再统一 `writev` | 每个 hook 记录 wire 边界，partial write 按实际消费字节依次触发 send-done |
| `Http2HeadersFrameEncoder.cpp:199-208` | 大 header block 拆 N 个 CONTINUATION 帧时每帧 `IoBuf::allocate` | overflow 缓冲改从 IoBufNodePool 缓存取，避免全局分配器 |
| `Http2HpackEncoderIoBufWriter.cpp:110` | ✅ 已修复：`commit_output` 每次 `first_writable()` O(n) 遍历链，缓存了 `tail_` 却不用 | `if (tail_==nullptr \|\| tail_->writable()==0) tail_=block_.first_writable();` 否则跳过（3 行） |
| `Http2HpackDecoder.cpp:224` / `Http3QpackDecoder.cpp:250-265,306` | 解码器原始字面量双重拷贝：数据->scratch->pool | 单 chunk 整串（`take==remaining && string_received_==0 && !huffman`）时直接把源指针传回调，省第二次 memcpy |
| `Http2HpackDecodeTable.cpp:125` | `insert` 重算 name hash，上游 `FieldView::name_hash` 已有 | `insert` 加 `name_hash` 参数透传 |
| `HttpHeaders.cpp` | ✅ 已修复：`add()` 对 name 做两次 pool 分配（original + lowercase，各 `name.size()` 字节） | 单次分配 `2*name_len` 缓冲，original 在 `[0,len)`、lowercase 在 `[len,2*len)`；H1/H2/H3 请求头热路径走 `add_view` 不受影响，主要惠及 trailer 与 `set()` |
| `HttpHeaders.cpp` | ✅ 已修复：`set()` 哈希 name 两次（`remove` 一次 + `add` 一次） | 先准备完整 owned field，再用其中的 lowercase/hash 删除旧字段并挂链；分配失败保留旧值 |
| `HttpHeaders.cpp` | ✅ 已修复：lowercase 预哈希查找对已小写两侧仍走逐字节大小写折叠 | 预哈希 API 收紧为 lowercase 契约并走 `string_view::operator==`；普通 API 保留大小写不敏感路径 |
| `Http1ExchangeIo.cpp` / `ClientHttp1Exchange.cpp` | ✅ 已修复：trailer 解析忽略 parser 已算的 `line.header_hash`/`line.lowcase_header` | server request 与 client response trailer 均复用 parser hash；短名称复用 lowercase cache，长名称只补 lowercase |
| `HttpHeaders.cpp` | ✅ 已修复：`rebuild_buckets()` 死代码，桶固定 32 从不扩容 | 桶改为 pool 裸指针数组；`link_field` 在负载因子达到 1 时 best-effort 倍增 rehash，失败继续使用原桶 |
| `Http1Connection.cpp:99-115` | ✅ 已修复：`read_into_inbound` 死代码 | 删除 |
| `HttpTransport.cpp:433` | ✅ 已修复：`negotiated_alpn()` 返回 `std::string`，存在不必要的拥有型复制 | 返 `string_view`（ALPN 字节存活于 SSL 对象内）；`TlsStreamFd::selected_alpn` 对应改 |
| `HttpHeaderHash.h:20` | 哈希乘子 31、32 位截断，分布偏弱 | 换 FNV-1a 64（`0x100000001b3ull`，初值 `0xcbf29ce484222325ull`）；32 桶下影响有限 |
| `Http2Connection.cpp:1267-1276,1482-1489` | ✅ 已修复：GOAWAY 选择性关闭和全量 teardown 原先用 `new Http2Stream*[n]` 收集指针 | 直接遍历 `owned_stream_list_`；操作当前 stream 前预取 `next`，用 `Lease` 固定当前对象生命周期，删除两处分配及 `NoMem` 分支 |
| `Http2Connection.cpp:938-965` | ✅ 已修复：`apply_peer_initial_stream_window` 原先两次全表 `for_each` | 在同一次遍历中用 64 位值校验并立即更新；越界错误由 SETTINGS 路径作为连接级致命错误传播，无需回滚已更新 stream，且仅在全遍成功后提交 `peer_initial_stream_send_window_`。新增多 stream 增减、溢出和摘链遍历测试；2026-07-13 全量 1126 ctest 通过 |
| `Http2Connection.cpp:926-974` | ✅ 已修复：控制帧（WINDOW_UPDATE/RST/GOAWAY）先编码进栈数组再 memcpy 进 slab | 直接编码进 `dst + kFrameHeaderSize`，省双写 |
| `Http2DataFrameEncoder.cpp:58-61` | ⏸ 已核查，暂不处理：9 字节帧头确实经栈数组再由 `append_copy` 写入输出，但 payload 是外部 `IoBufChain`；为保持 payload 零拷贝，帧头仍需独立且存活到异步发送完成。多帧时首帧 payload 进入 `tail_chain` 后，后续 `reserve_slot(9)` 会返回 `Invalid`，而 `append_copy` 还承担分配小 `IoBuf` 的回退语义 | 不能直接替换为 `reserve_slot(9)` + 原地编码。单次 9 字节复制收益很低；若 profiling 显示瓶颈，优先考虑 header slab/小对象池，或增加保留 fallback 语义的直接写入 API，优化后续帧头的小额分配 |
| `HttpExchange.h:148` | `HttpHandler = std::function<...>` | 每连接一次（非每请求），低优先；可换带 SBO 的类型擦除 callable |
| `Http3QpackEncoder.cpp:90-117` | ✅ 已修复：伪头编码器走通用 `find()` 而非直接静态下标（见 #2） | 同 #2 |
| `Http3QpackEncoderIoBufWriter` / `Http2HpackStaticTable.cpp` | 静态表 `find` 对已小写名仍走逐字节 ci 比较 | 若调用边界保证 name 已小写，可改用 `same_bytes` |

---

## 四、核查后确认干净（不用动）

- **Huffman 两级表**（`Huffman.cpp`）：root16 快路径 + per-state byte 表，标准高性能设计；双遍算 encoded_length 是 HPACK 长度前缀格式所必需。
- **`HttpUriParse.cpp`**：单遍字节扫描 + 位图表；简单 URI 零分配返 `string_view`，复杂 URI 单次 pool 分配。
- **`HeaderMap.h`**：每实例都是一次性 `static` 表（`request_header_ref_map`/`hop_by_hop_header_map`/pseudo/regular handler maps），<10 项。
- **连接池分片与无锁偷取**：`Local/StealableHttp1ConnectionPoolSet` 按 loop 分片；跨分片偷取与远程归还走 `EventLoop::post`（MpscQueue）非 mutex；`AcquireAwaiter` 栈上 + 侵入式 `NotifyEntry`。`Http1ConnectionGroupHintTable` 是固定大小无锁原子计数指纹滤波器。
- **H2 stream table**（`Http2StreamTable.cpp`）：O(1) 开放寻址 + 后移删除。
- **body 重组**（`detail/Http2BodyRecvState.cpp`）：零拷贝 `take_prefix` 移节点。
- **`Http2SendAwaiter.h`**：awaiter 居协程帧内，侵入式 `NotifyEntry`/`TimerEntry`，裸函数指针 ops。
- **H2 编码路径**（`ClientHttp2Request.cpp`/`ServerHttp2Request.cpp`）：headers 走 `begin()/end()` + `string_view`，payload 零拷贝 `take_prefix`。
- **H3↔QUIC 边界**：recv 零拷贝（`QuicStream::read` -> `retain_slice` -> `take_prefix`）；send 走 scatter-gather（`try_write(IoBufChain&)` + `bind_or_migrate_chain_nodes` 跨池重链不拷数据）。
- **`Http1ConnectionPoolCore.cpp`**：全用 `loop_->now()`；entry/bucket free-list；侵入式空闲链；`park_entry`/`acquire` 同 loop 无锁。
- **body 读路径**（`Http1ExchangeIo.cpp:219-340`、client `:440-495`）：`ensure_read_buf_writable` 在 `unique()` 且容量足时复用 `read_buf_`；`retain_slice`/`take_prefix` 零拷贝。

---

## 建议的修复顺序

按性价比排序，前几项改动小、风险低、收益大：

1. **#1 chunked write 合并**——最大最直接，流式响应路径。
2. **#3 Huffman size 判定**——两个编码器一起，零风险零成本。
3. **#6 HttpHeaders 加 hash 重载**——根因，惠及全协议调用点。
4. **#7 HttpExchange 暴露已解析值**——代理热路径根因。
5. **#2 QPACK 静态表索引化 + 伪头直连**——H3 每请求出站头。
6. **#4 H2 读缓冲复用 / #5 H3 scratch 复用 / #10 H1 parse buffer 复用**——三个同类的"每请求裸 malloc"问题，思路一致，可一起做。
7. 第三节低优先项按需推进。
