# TLS 自研实现 · 12 已连接阶段读路径重做(连续 wire 缓冲 + 批量开记录 + TlsConnection 去 pimpl)

状态:§3 拆分器(c55ab263)、§4–§5 读路径(a4612e5d)、§4.3 去 pimpl(23a73445)已实现;bench 见 §11。范围仅 TCP 面已连接阶段(`TlsStreamFd` 读路径 +
`TlsConnection`);握手路径(`TlsHandshakeContext`/`TlsRecordReader`/
`TlsRecordCipherChain`)与写路径不动。

## 1. 动机(现状实测)

数据来自当前构建(clang 22 + libc++,x86_64):

| 对象 | 大小 |
|---|---|
| `TlsConnection`(外壳,只有 `impl_`) | 8 B |
| `TlsConnection::Impl` | 20200 B,其中 `open_scratch_` 18432 B |
| `Impl` 去掉 scratch | ≈ 1768 B(两个 `TlsRecordCipher` 各 608 B、`alpn_` 256 B、3 条 chain 等) |
| `TlsStreamFd` | 448 B(`TlsTransport` 在 accept 时整体分配) |

1. **大流量读放大**:wire 每次固定读 32 KiB(`kReadChunk`),1.3 满 record 在线上
   16406 B(5+16384+1+16),32768/16406 ≈ 2,几乎每次读都有一个 record 跨 chunk 边界。
   跨界 record 不满足 `tls_record_open_in_place` 的连续条件,走 transcribe:整条
   record(≈16.4K)拷进 `open_scratch_` 解密,`deliver_plaintext` 再 allocate +
   memcpy 16K 明文。大流量下约等于每收 1 字节多拷 1 字节,外加一次分配。
2. **逐 record 的 chain 开销**:`TlsRecordReader::next()` 每条都要 `fill_write_iov`
   偷看 header、`take_prefix`(节点池分配 + 边界节点 `retain_slice`)、`consume`;
   `open_in_place` 里 `chain_contiguous` 再走一遍 chain。
3. **常驻内存与分配**:每连接 `open_scratch_` 18 KiB 常驻,且 `open_scratch_{}` 值初始化
   每连接 memset 18 KiB(与 767079a3 去掉的握手 scratch 零初始化同类)。握手完成时
   两次分配(8 B 外壳 + 20 KiB Impl)。
4. **pimpl 已无收益**:`TlsConnection.h` → `TlsConnectedState.h` → `TlsRecordCipher.h`
   → `<openssl/aead.h>`,头文件注释 "this header pulls no OpenSSL" 早已不成立;
   代价是每个公共方法一次 `impl_ == nullptr` 判断(违反 CLAUDE.md "Nullability at
   Edges"),以及一个半构造态——Impl 分配失败时 `conn_` 非空,`install_connection`
   照样返回成功,错误推迟到读时以 Fatal 出现。

## 2. 定谳

1. **已连接阶段的每条 record 都在一块连续 `IoBuf` 里拆分、原地解密**。record 永不跨
   节点 → 不再有 transcribe 分支,`open_scratch_` 删除。
2. **wire 读缓冲容量** = `clamp(size + 200, kInboundMinCapacity, kInboundMaxCapacity)`,
   `size` 是调用方请求的明文量(截断到 16K 之前的值):
   - `kInboundMaxCapacity = 64 KiB − 16`;
   - `kInboundMinCapacity = 20 KiB − 16`,必须 ≥ 5 + `kTlsMaxCiphertextRecordSize`
     (= 18437,拆分器接受的最大 record)——否则一条拆分器放行、缓冲区装不下的 record
     永远凑不齐,连接卡死;
   - 减 16 是 `IoBuf` 的 ControlBlock,让总分配恰好落在 64 KiB / 20 KiB size class
     (jemalloc 下 `allocate(65536)` 会落到 80 KiB 档;现在的 32 KiB chunk 也落在 40 KiB 档);
   - +200 只在 size ∈ (≈20K, ≈64K) 时起作用,约等于 9 条 1.3 record 的开销。
   - 效果:H2(`read_buffer_size` 默认 64K)每次读 64K;H1 读 header 落在下界;与
     `TcpTransport::try_readv` 的 `allocate(size)` 行为一致。
3. **拆分在 glue(`TlsStreamFd`)**,纯函数拆分器每批最多 32 条,描述符是相对 wire 缓冲
   的 offset;连接用 `on_records(wire, batch)` 按序处理。
4. **不完整的尾部 record 以 `IoBuf` 存在 `TlsStreamFd::inbound_`**(原 wire 缓冲的视图)。
   下次读时,若原缓冲剩余的 tailroom 还装得下这条 record,就直接读进 tailroom(不拷贝、
   不分配);否则拷到新缓冲区开头,拷贝量 ≤ 一条 record。
5. **`TlsRecord`/`TlsRecordReader`/`TlsRecordCipherChain` 不改**:握手路径(上下文、
   Tls12/13 握手、两个引擎)继续用。已连接阶段新增 `TlsRecordSpan` + 拆分器。
6. **拆分器继续用统一上限** `kTlsMaxCiphertextRecordSize`(与现 reader 相同),各版本的
   精确上界仍由连接在 open 前检查 → 告警码行为不变(1.3 上超长 record 仍是
   bad_record_mac,不改成 record_overflow)。
7. **`TlsConnection` 去 pimpl**,成员按值持有;`TlsStreamFd::conn_` 改为
   `std::optional<TlsConnection>`。必须在 1 之后做:先去掉 18 KiB scratch,按值内联才
   没有顾虑。
8. **`try_read` 对调用方的契约不变**(每次最多交付 16K 明文),只是 wire 缓冲大小改用
   截断前的 size。放开交付上限另行处理(§9)。

## 3. 新增:record 拆分器

`include/fiber/tls/record/TlsRecordFramer.h` + `src/tls/record/TlsRecordFramer.cpp`:

```cpp
// One record framed off a contiguous wire region: payload bytes are
// region[offset, offset + length) (the 5-byte header precedes them).
struct TlsRecordSpan {
    std::uint32_t offset = 0;
    std::uint16_t length = 0;
    std::uint16_t legacy_version = 0;
    TlsContentType type = TlsContentType::ApplicationData;
};

inline constexpr std::size_t kTlsRecordBatchMax = 32;

struct TlsFrameResult {
    std::size_t count = 0;    // records written to out
    std::size_t consumed = 0; // wire bytes those records cover, headers included
    bool fatal = false;       // framing violation right after them
    TlsAlertDesc alert = TlsAlertDesc::CloseNotify;
};

// Frames complete records off the head of `wire` into `out`, in wire order.
// Stops at the first incomplete record (header or body), when `out` is full,
// or at a framing violation. Pure: no allocation, no IoBuf.
[[nodiscard]] TlsFrameResult tls_frame_records(std::span<const std::uint8_t> wire,
                                               std::span<TlsRecordSpan> out) noexcept;
```

规则与 `TlsRecordReader::next()` 一致(复用 `tls_decode_record_header`):
- 未知 content type → fatal `unexpected_message`;
- length > `kTlsMaxCiphertextRecordSize` → fatal `record_overflow`,**只要 header 到齐就判**,
  不等 body;
- `legacy_record_version` 只解析不拒绝;零长 record 照常放行。
- fatal 之前已拆出的 record 仍在 `count`/`consumed` 里:调用方先处理它们,再报 fatal
  (与现在逐条 `next()` 的顺序一致)。
- `wire.size()` ≤ 64K(加上握手 leftover gather 也远小于 4G),`uint32_t` offset 足够;
  `FIBER_ASSERT(wire.size() <= UINT32_MAX)`。

## 4. TlsConnection

### 4.1 API 变化

```cpp
// 删除
void on_record(TlsRecord &&record) noexcept;

// 新增
// Complete records framed off one contiguous wire buffer, in wire order;
// offsets are relative to wire.readable_data(). Each record is opened in place
// inside `wire` (only its own bytes are written — no unique() requirement);
// app-data plaintext is delivered as wire.retain_slice() views (zero copy,
// atomic refcount, may leave the thread later). Processing is sequential and
// stops at the first terminal (ours or the peer's): the rest of the batch drops.
void on_records(mem::IoBuf &wire, std::span<const TlsRecordSpan> records) noexcept;
```

`on_framing_fatal`、`read`/`take`、`write`/`close_notify`/`take_output` 以及状态查询不变。

### 4.2 每条 record 的处理

```cpp
void TlsConnection::on_records(mem::IoBuf &wire, std::span<const TlsRecordSpan> records) noexcept {
    for (const TlsRecordSpan &record : records) {
        if (failed_ || peer_closed_) {
            return; // terminal: the rest of the batch drops
        }
        FIBER_ASSERT(record.offset + record.length <= wire.readable());
        route_record(wire, record);
    }
}
```

- `route_record`:分发逻辑不变,包括 warning 计数复位规则。1.3 的明文 alert(外层
  Alert、2 字节)直接 `on_alert_bytes(payload)`,`gather_small` 那条兜底分支消失。
- `open_and_route`:
  ```cpp
  std::uint8_t *payload = wire.readable_data() + record.offset;
  // 长度界先行(不变):越界 → bad_record_mac
  const std::size_t nonce = read_cipher_.kind() == TlsRecordProtectionKind::Tls12
                                    ? read_cipher_.explicit_nonce_len() : 0;
  std::uint8_t *plain = payload + nonce;
  const auto open = read_cipher_.open(record.type, record.legacy_version, record.length,
                                      {payload, record.length},
                                      {plain, read_cipher_.open_output_size(record.length)});
  // 错误映射不变:Overflow → record_overflow,其余 → bad_record_mac
  // 内层类型分发:Alert(plain_len == 2)/ AppData → deliver_plaintext(wire, record.offset + nonce, plain_len)
  //             / Handshake → feed_handshake_fragment(plain, plain_len)
  ```
  直接用 `TlsRecordCipher::open()` 的连续形式,它的就地别名约定(1.3:dst == ct;1.2:
  dst == ct + explicit_nonce_len)正好对应这个布局,GCM(+8)、ChaCha(+0)都覆盖。
  未合并的 `feat/tls-legacy-client-suites` 分支保留了同一约定(CBC 的 IV 16 B 就是
  explicit_nonce_len,dst 容量覆盖它剥掉的 MAC + padding),所以这条路径对 CBC 不用
  特殊处理;而 chain 适配器为 CBC 专门引入了 `detached_tag_len`。
- `deliver_plaintext(wire, offset, len)`:`len == 0` 直接返回;否则
  `plaintext_.append(wire.retain_slice(offset, len))`,append 失败 → fatal
  internal_error。使用带原子计数的 `retain_slice`,不用 `unsafe_retain_slice`:明文交付
  后可能跨线程。
- post-handshake 重组(`reassembly_`/`message_buf_`/`append_fragment`)不变:它本来就
  拷贝,不依赖 scratch。`gather_small` 只保留给 `reassembly_` 读 4 字节 header 用。

删除:`open_scratch_`、`kOpenScratchSize`、in_chain/transcribe 分支、
`TlsRecordCipherChain.h` include。

### 4.3 去 pimpl(独立提交,在 §4.2 之后)

- `struct Impl` 的成员与私有辅助函数并入 `TlsConnection` 的 private 段。头文件新增
  `handshake/TlsHandshakeMessage.h`(为 `TlsHandshakeType`;很轻,不带 OpenSSL)和
  `record/TlsRecordFramer.h`。
- 删掉所有 `impl_ == nullptr` 分支:`failed()` 就是 `failed_`,`alpn()` 直接返回 span,
  `write`/`close_notify` 不再有 NoMem 的空指针返回。构造不会失败。
- 头文件注释:framing 一节改成"glue 用 `tls_frame_records` 在一块连续 wire 缓冲上拆分,
  批量交给 `on_records()`";删除 "pimpl … pulls no OpenSSL"。
- 实测 `sizeof(TlsConnection)` = 1776 B。仍然是 NonMovable,`std::optional::emplace` 原地构造。

## 5. TlsStreamFd

### 5.1 成员与常量

```cpp
// 头文件
std::optional<tls::TlsConnection> conn_; // the connected phase
// Connected-phase wire bytes not yet framed: empty, or ONE incomplete record
// (the tail of the last wire read; a view into that read's buffer). Never holds
// a complete record once process_inbound() returns.
mem::IoBuf inbound_{};
// 删除: tls::TlsRecordReader record_reader_;(以及 TlsRecordReader.h include)
```

```cpp
// .cpp
constexpr std::size_t kInboundMaxCapacity = 64 * 1024 - mem::kIoBufControlBlockSize;
constexpr std::size_t kInboundMinCapacity = 20 * 1024 - mem::kIoBufControlBlockSize;
constexpr std::size_t kInboundSlack = 200; // per-read record overhead allowance
static_assert(kInboundMinCapacity >= tls::kTlsRecordHeaderSize + tls::kTlsMaxCiphertextRecordSize);
```

`mem::kIoBufControlBlockSize`:`IoBuf.h` 新导出的常量,`IoBuf.cpp` 现有的
`static_assert(sizeof(ControlBlock) == 16)` 改为针对它断言。

`kReadChunk`(32K)只留给握手的 `read_handshake_chunk`。

### 5.2 读路径

```cpp
IoResult<std::size_t> TlsStreamFd::try_read(std::size_t size, mem::IoBufChain &out) noexcept {
    // ... BadFd / size == 0 不变
    const std::size_t wire_hint = size;      // wire sizing follows the caller's appetite
    size = std::min(size, kRecordPlaintextMax); // delivery cap unchanged (§9)
    for (;;) {
        // early_data_ / conn_->take(...) 不变
        // NeedMore:
        const IoErr err = read_wire(wire_hint);
        if (err != IoErr::None) {
            return std::unexpected(err); // WouldBlock included
        }
    }
}

IoErr TlsStreamFd::read_wire(std::size_t hint) noexcept {
    const std::size_t carry = inbound_.readable(); // < one record (invariant I1)
    if (carry > 0 && carry + inbound_.writable() >= incomplete_record_size(inbound_)) {
        // continue the record in its own buffer's tailroom: no copy, no allocation
        // read into inbound_.writable_data(); EOF → ConnReset; commit; process_inbound()
    }
    mem::IoBuf wire = mem::IoBuf::allocate(
            std::clamp(hint + kInboundSlack, kInboundMinCapacity, kInboundMaxCapacity));
    if (!wire.valid()) {
        return IoErr::NoMem;
    }
    // Read past the carry slot first: a WouldBlock costs no carry copy.
    auto got = stream_fd_.try_read(wire.writable_data() + carry, wire.writable() - carry);
    if (!got) {
        return got.error(); // inbound_ untouched
    }
    if (*got == 0) {
        return IoErr::ConnReset; // EOF without close_notify: truncation
    }
    if (carry > 0) {
        std::memcpy(wire.writable_data(), inbound_.readable_data(), carry);
    }
    wire.commit(carry + *got);
    inbound_ = std::move(wire); // the old storage lives on only through delivered slices
    return process_inbound();
}

IoErr TlsStreamFd::process_inbound() noexcept {
    std::array<tls::TlsRecordSpan, tls::kTlsRecordBatchMax> batch;
    for (;;) {
        const tls::TlsFrameResult framed =
                tls::tls_frame_records({inbound_.readable_data(), inbound_.readable()}, batch);
        if (framed.count > 0) {
            conn_->on_records(inbound_, {batch.data(), framed.count});
            inbound_.consume(framed.consumed);
        }
        if (conn_->failed() || conn_->peer_closed()) {
            break; // terminal: the rest drops (the read path surfaces it)
        }
        if (framed.fatal) {
            conn_->on_framing_fatal(framed.alert);
            break;
        }
        if (framed.count < batch.size()) {
            if (inbound_.readable() == 0) {
                inbound_ = {}; // nothing pending: pin no storage while idle
            }
            return IoErr::None; // stopped at an incomplete tail
        }
    }
    inbound_ = {};
    return IoErr::None;
}
```

要点:
- **tailroom 续读**(实现时加入):`incomplete_record_size` 在 header 到齐时是 5 + length,
  否则是 5。若只走"拷 carry 到新缓冲"一条路,一条 record 分多个小段到达(慢链路、读方
  每段都被唤醒)时,每次读都要重拷不断变长的前缀——16K record 按 1448 B 一段到达约 6 倍
  拷贝,外加每段一次 20–64K 分配,比现在还差。续读让这种情况零拷贝、零分配;只有缓冲已满
  (大流量)时才走拷贝。tailroom 在所有已交付 slice 的视图之外,写它不碰共享字节。
- 一次 `read_wire` 处理完缓冲里**所有**完整 record(多批循环),和现在
  `drain_records` 的"读一次排空一次"相同;`has_pending_read()` 不用改(I1)。
- 先查终态再查 fatal:一批里先出现 close_notify、后面又是坏 header 时,不会再调
  `on_framing_fatal`——与现在逐条处理时"peer_closed 后 next() 不再被调用"一致。
- `wire.writable() - carry > 0` 恒成立:容量 ≥ min ≥ 一条完整 record > carry。

### 5.3 安装连接与握手 leftover

```cpp
IoErr TlsStreamFd::install_connection(tls::TlsConnectionRole role, tls::TlsConnectedState &&state,
                                      mem::IoBufChain &&leftover) noexcept {
    conn_.emplace(role, std::move(state)); // cannot fail: no NoMem branch any more
    const std::size_t bytes = leftover.readable_bytes();
    if (bytes == 0) {
        return IoErr::None;
    }
    const mem::IoBuf *front = leftover.first_readable();
    if (front->readable() == bytes) {
        inbound_ = *front; // usual case: a view of the engine's last read chunk, framed in place
    } else {
        // gather once into a fresh buffer of `bytes`; NoMem → return
    }
    return process_inbound();
}
```

在 leftover 视图上原地解密是安全的:只写本 record 自己的字节,与现在 `open_in_place`
作用在 leftover 上相同;early data 的明文在另外的区域。

### 5.4 其余机械改动

- `conn_ == nullptr` → `!conn_`;`delete conn_; conn_ = nullptr;` → `conn_.reset()`。
- `close()`:`record_reader_.reset()` → `inbound_ = {}`。
- `handshake_step` 里的 `if (conn_ == nullptr)` → `if (!conn_)`。
- detach/adopt:`inbound_` 是 IoBuf,不依赖节点池,无需处理(比 chain 简单)。

## 6. 不变式与边界

- **I1**:`process_inbound()` 返回后,`inbound_` 为空或只含一条不完整 record → carry ≤
  18436 < `kInboundMinCapacity`。
- **I2**:已连接阶段拆出的每条 record 都在一块连续 IoBuf 内 → 总是原地 open,不需要 scratch。
- **I3**:交付的明文 slice 持有 wire 存储(原子计数),wire 句柄先释放没有影响。
- **同一批里的 KeyUpdate**:拆分只看 header,和密钥无关,提前拆 32 条是安全的;顺序
  处理保证 KeyUpdate 之后的 record 用新的 `read_cipher_` 解密。
- **批中途终态**:剩余 record 与 `inbound_` 一并丢弃并释放。现在是留在 reader 里不再处理,
  改后提前释放。
- **WouldBlock**:新缓冲直接丢掉,`inbound_` 不动,没有 carry 拷贝。分配一次再丢掉的
  浪费和现在的 32K 相同(§9 有后续优化)。
- **EOF**:无论有没有 carry 都返回 ConnReset,不变。
- **明文钉住缓冲**:一个存活的明文 slice 钉住整块 wire 缓冲。现在是 32K(jemalloc 下
  40K),H2 下变为 64K;与 TCP 路径的 `allocate(size)` 一样。

## 7. 收益汇总

| | 现在 | 改后 |
|---|---|---|
| 每连接常驻 | 448 B + 8 B + 20200 B(另外 2 次分配,18K memset) | 实测 `TlsStreamFd` 2216 B / `TlsTransport` 2264 B,一次分配 |
| 握手完成时的分配 | 2 次 | 0 次 |
| 每次 wire 读 | 固定 32K | clamp(size+200, 20K, 64K);H2 为 64K,recv 次数减半 |
| 跨界 record | 拷整条进 scratch + 分配 + 拷明文(≈ 每收 1 字节多拷 1 字节) | 不存在;缓冲有余量时续读零拷贝,缓冲已满时最多拷一条不完整 record(大流量平均约半条 / 64K ≈ 12%) |
| 每条 record 的拆分 | take_prefix(节点分配 + 边界 retain)+ 偷看 header + consume | 12 B 描述符;app data 只有一次 `retain_slice` + 一个节点 |

`TlsTransport` 在 accept 时多预留约 1.8 KB(握手期间未用)。代价可接受,远小于把 20K
scratch 内联的方案。

## 8. 文件清单与测试

**新增**
- `include/fiber/tls/record/TlsRecordFramer.h`、`src/tls/record/TlsRecordFramer.cpp`(自动 glob)
- `tests/TlsRecordFramerTest.cpp`

**修改**
- `include/fiber/common/mem/IoBuf.h`、`src/common/mem/IoBuf.cpp`:导出 `kIoBufControlBlockSize`
- `include/fiber/tls/TlsConnection.h`、`src/tls/connection/TlsConnection.cpp`
- `include/fiber/net/detail/TlsStreamFd.h`、`src/net/detail/TlsStreamFd.cpp`
- `tests/TlsConnectionTest.cpp`、`tests/TlsStreamFdTest.cpp`
- `fuzz/tls_connection_fuzzer.cpp`:改用拆分器 + carry,**输入格式不变**,corpus 不用重新生成

**TlsRecordFramerTest**
- 空 / 不足 5 字节 / 只有 header / header + 部分 body → count 0、consumed 0、非 fatal
- 一条完整 record;完整 + 不完整;offset/length/type/version 正确
- 33 条 → 第一次 32 条,第二次 1 条(批上限)
- 第 k 条是未知类型 → count k,fatal unexpected_message
- length = 上限时接受(body 未到也只是不完整);上限 + 1 → header 一到就 record_overflow
- 零长 record 正常拆出

**TlsConnectionTest**
- `WireFeeder` 改为 carry IoBuf + 拆分器 + `on_records`(镜像 glue);现有 21 个用例原样通过
- 新增:多 record wire(含 16K record)在每个字节位置切成两次投喂 → 明文一致
- 新增:KeyUpdate 和其后用新密钥的 record 在同一批
- 新增:批中途 close_notify / 致命 alert → 之后的 record 被丢弃
- 新增:先有正常 record、后有坏 header → 正常 record 先交付,再 Fatal
- 新增:wire 句柄先释放,之后 take/read 的明文仍完整
- 新增:1.2 ChaCha 的合成对(explicit nonce 0;现有 Synthetic12 只有 GCM 的 +8)

**TlsStreamFdTest**(真实 socket,`WirePath` 中继线程控制字节怎么到达服务端)
- `BulkReadCarriesIncompleteRecordsAcrossWireBuffers`:1 MiB,调用方 size 4K 与 64K → 走拷贝 carry
- `TinySegmentsReassembleRecordsAcrossReads`:中继按 1–7 字节小段转发 → 走 tailroom 续读(含 header 被切开)
- `AppDataBehindClientFinishedIsReadableAfterHandshake`:中继攒批转发,Finished 与应用数据同一次到达
  → 单节点 leftover adopt,握手返回时 `has_pending_read()` 已为真
- 实现时用临时探针确认过三条路径都被命中(拷贝 84 次 / 续读 328 次 / leftover 1 次),探针未提交

**验证命令**
```bash
cmake --build build && ./build/fiber_tests --gtest_filter='Tls*' && ctest --test-dir build
# fuzz 树:按 worktree-build-isolated-deps 的做法使用独立 deps 目录
./build-fuzz/fuzz/tls_connection_fuzzer -runs=0 fuzz/corpus/tls_connection_fuzzer
# 探索性运行一定先给 scratch 目录,避免把新输入写进 fuzz/corpus
./build-fuzz/fuzz/tls_connection_fuzzer <scratch>/out fuzz/corpus/tls_connection_fuzzer -runs=200000
```
性能:temp/bench,对比 TLS 下 H1/H2 的 1m 大响应和小响应,前后各测一次。

## 9. 提交拆分

1. `feat(tls): frame records off a contiguous wire buffer`:拆分器 + `TlsRecordSpan` +
   `TlsRecordFramerTest`,暂时没有调用方。
2. `perf(tls): open connected-phase records in place in one wire buffer`:
   `on_records` 取代 `on_record`,删除 `open_scratch_`;`TlsStreamFd` 的 `read_wire`/
   `process_inbound`/`inbound_`/leftover adopt;`kIoBufControlBlockSize`;测试与 fuzzer
   harness 同步。删掉 `on_record` 会让 glue 编译不过,所以这些必须在同一个提交里。
3. `refactor(tls): hold TlsConnection state by value`:去 pimpl + `std::optional<TlsConnection>`
   + 头文件注释修正。
4. 跑 bench,结果记到本文。

## 10. 不在本方案(后续候选)

- **放开 `try_read` 的 16K 交付上限**:`plaintext_` 现在可能有多条 record 的明文,H2 一次
  读 64K 要调 4 次 `try_readv`,放开后 1 次。需要先逐个确认调用方不假设"每次只追加一个节点"。
- **fd 已确认读空时跳过 wire 读**:短读之后 `RWFd` 的读状态是 Blocked,H2 每次排空最后
  那次 `try_read` 会白白分配一块缓冲 + 一次 EAGAIN 的 syscall。读状态为 Blocked 时可以
  直接返回 WouldBlock(Unknown 仍然要试读)。TCP 路径同样适用,单独评估。
- **carry 时复用缓冲**:`inbound_.unique()` 且容量够时 memmove 到开头复用,省掉一次分配。
  不保留空缓冲(否则每个空闲连接都要钉住 64K)。
- **写侧 `write_scratch_`**(每连接懒分配 16K):改成每线程 scratch;H2 DATA 帧 9 字节
  header 节点导致的合并拷贝也在写侧,另案处理。
- **握手迁移到同一个拆分器**:`TlsHandshakeContext` 的 `open_scratch_` 只在握手期间存在
  于协程帧中,不常驻,优先级低。
- **小 record 明文压缩**:把相邻的小明文 memmove 到一起以减少节点,只有 bench 显示节点
  开销明显时再做。
- **与 legacy CBC 分支的冲突**:只有 `TlsConnection.cpp` 里两处 `init()` 多一个
  `TlsRecordDirection` 参数,合并时机械处理即可。

## 11. Bench(2026-10-01,WSL2 i7-13700H)

**方法**
- before = d4bdde31(本系列之前),after = 23a73445。两者都用 `git archive` 导出到 `temp/ab/<v>-src`,
  以相同配置构建(Release、libc++、LTO、clang-22,各自独立 deps 目录),只换 lite_nginx 二进制;
  backend 共用。
- temp/bench 的 lite_nginx 代理场景:lite 4 个 worker 绑 CPU 4–9,压测工具绑 10–15,backend 绑 0–3,
  每轮 20 s,4 个样本,before/after 顺序逐样本交替(`ab_tls.sh`,`ab_parse.py` 汇总)。
- h2 = h2load `-t6 -c32 -m16`(TLS);h1s = wrk `-t6 -c256` 走 TLS 端口(HTTP/1.1,无 ALPN)——
  harness 原有的 `h1` 是明文,不经过 TLS。
- CPU/req = 代理进程 utime+stime(全部线程)÷ 成功请求数。

**结果**(中位数;64 轮,0 错误)

| 协议 | 场景 | RPS before | RPS after | Δ RPS | CPU µs/req before | after | Δ CPU/req |
|---|---|---:|---:|---:|---:|---:|---:|
| h1s | get1k | 135,920 | 137,913 | +1.5% | 29.3 | 28.9 | −1.2% |
| h1s | get64k | 55,865 | 56,407 | +1.0% | 71.3 | 70.8 | −0.7% |
| h1s | get1m | 5,605 | 5,633 | +0.5% | 713.7 | 709.9 | −0.5% |
| h1s | post1m | 2,836 | 2,856 | +0.7% | 1,411.6 | 1,400.8 | −0.8% |
| h2 | get1k | 189,301 | 189,609 | +0.2% | 21.0 | 20.9 | −0.4% |
| h2 | get64k | 38,858 | 39,149 | +0.7% | 102.3 | 101.8 | −0.5% |
| h2 | get1m | 3,668 | 3,615 | −1.5% | 1,092.0 | 1,105.2 | +1.2% |
| h2 | post1m | 1,695 | 1,738 | **+2.5%** | 2,356.9 | 2,294.2 | **−2.7%** |

h2 get1k 一行是追加的 8 样本复测:4 样本时中位数是 −2.7%,但单样本波动约 ±10%(before 第一轮
226k 是离群值);8 样本下两边持平。

**解读**
- 代理场景里,只有 POST 的请求体是大块数据经过代理的 TLS 读路径;GET 场景经 TLS 读进来的只是小请求,
  1 MiB 响应走的是 TLS 写路径,本系列没有改。
- h2 post1m 是唯一明确的变化:+2.5% RPS,4 个 after 样本(1,730–1,751)全部高于 4 个 before 样本
  (1,630–1,701);每请求省约 63 µs CPU,与去掉约 1 MiB 的跨界双拷贝量级相符。
- h1s post1m 只有 +0.7% / −11 µs,比 h2 小得多,原因未查(可能与 H1 请求体读取时传入的 size 有关,
  未验证)。
- 其余场景 |Δ| ≤ 1.5%,在噪声范围内,没有回退。
- 每连接常驻内存从约 20.6 KB(3 次分配)降到 2.3 KB(1 次分配),这在吞吐测试里看不出来,
  影响的是连接数规模下的内存占用。
