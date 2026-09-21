# TLS 自研实现 · 03 记录层：IoBufChain → TlsRecord 零拷贝拆分（Reader + Writer）

日期：2026-09-20（同日设计演化：借用视图 → take 移交；同日补 Writer）
分支：`tls`
状态：**分帧 Reader + Writer + RecordCipher（含链形态适配层）均已完成**（保护见 05 号）

## 1. 交付物

| 文件 | 内容 |
|---|---|
| `include/fiber/tls/TlsTypes.h` | `TlsAlertDesc`（RFC 8446 §6 全表，去掉保留值）、`TlsAlertLevel` |
| `include/fiber/tls/record/TlsRecord.h` | `TlsContentType`、尺寸常量、`TlsRecordHeader` + `tls_decode_record_header`、**持有 payload 链的 `TlsRecord`**（含 `contiguous_payload()` 快路径） |
| `include/fiber/tls/record/TlsRecordReader.h` | 流式分帧器（pull 模型，take 语义） |
| `src/tls/record/*.cpp` | 实现（reader 仅 ~80 行） |
| `tests/TlsRecordReaderTest.cpp` | 15 用例 |

CMake 零改动（`GLOB_RECURSE` 自动纳入）。

## 2. API 形状

```cpp
TlsRecordReader reader(node_pool);        // 或默认构造 + bind_node_pool()
reader.feed(mem::IoBuf &&);               // 追加入站字节
reader.feed(mem::IoBufChain &&);
auto r = reader.next();                   // take 出一条记录
// r.status ∈ { NeedMore, Ok, Fatal }
//   Ok: r.record 拥有自己的字节（payload 为 IoBufChain）
//   Fatal: r.alert —— 应发送的告警
reader.pending_bytes();
reader.reset();
```

```cpp
struct TlsRecord {
    TlsContentType type;
    std::uint16_t legacy_version;
    std::uint16_t length;          // == payload.readable_bytes()
    mem::IoBufChain payload;       // 拆分好的原始字节（跨节点即多节点链）

    const std::uint8_t *contiguous_payload() const noexcept;
    // 单 span 覆盖全 payload 时返回连续指针（≈常态：记录落在一次 read 内）；
    // 跨节点或空记录返回 nullptr，消费者自行拷贝材料化
};
```

## 3. 关键设计决策

### 3.1 拆分 = take，永不拷贝 payload（三轮设计讨论的结论）
`next()` 用 `IoBufChain::take_prefix(5+len)` 把记录字节**移出** pending 链，随后在记录链上
`consume(5)` 跳过头部：

- 记录整节点结束 → 节点**整体移入**记录链（指针搬运，零 refcount、零拷贝）
- 记录边界切在节点中间 → 该节点做一次 `retain_slice` 零拷贝视图（+1 池节点分配），源节点 `consume` 前移
- 跨节点记录 = 原样的多节点链，**不 gather、不拷贝**；材料化是消费者的事

演化记录：v1 为借用视图（`const uint8_t*`，"下次 next() 前有效"契约 + reader 内 gather）；
讨论后确认——记录应携带所有权、分帧层语义应为"按 TLS 边界切好的原始字节"，连续性需求
（AEAD 单连续 src、握手消息线性解析）由消费者在解码时拷贝解决。比 v1 少一次拷贝：跨节点
握手记录从 gather+重组两次拷贝变为重组器直拷一次。

### 3.2 所有权与生命周期契约
- 记录独立于 reader 存活（有 `TakenRecordOutlivesReader` 测试固化），`retire_bytes_`/`gather_`
  等懒回收机制全部删除
- **on-loop 析构契约**：payload 链的节点来自 reader 的 `IoBufNodePool`（freelist 非线程安全），
  记录必须在拥有该池的 loop 上析构；离开连接/跨线程的字节由 pass-up 边界用 `retain_slice`
  重铸为普通 `IoBuf`（refcount 原子，池身份不出 tls 模块）。空间性契约（线程归属）比时间性
  借用契约更抗重构，且 TSan 可见——这是 H3 write_body node_pool mismatch 一类 bug 的教训

### 3.3 成本
每条记录：边界情形 2 次原子 refcount + 1 次 node pool freelist pop（整节点情形为零），
vs AEAD μs 级成本 <5%。`Result` 由此变 move-only（原 trivially_copyable 断言移除）。

### 3.4 校验边界（与密码状态无关的规则留在分帧层）
- 未知 content type（∉ {20,21,22,23}）→ Fatal `unexpected_message`（heartbeat 24 一并拒绝）
- `length > kTlsMaxCiphertextRecordSize`（2^14+2048，1.2/1.3 宽松者）→ Fatal `record_overflow`；
  1.3 的 2^14+256 精确上限、解密后明文上限属 RecordCipher 层
- `legacy_record_version` 只解析不拒绝（RFC 8446 §5.1 "MUST be ignored for all purposes"）
- 零长度记录放行（`contiguous_payload()==nullptr`，调用方先判 `length`）；CCS 必须 1 字节等
  类型规则归消费者
- 头部窥探：`fill_write_iov` 5 槽（5 字节至多跨 5 个非空 span），零消费即可校验

## 4. 测试覆盖（15 绿，全量 2113 绿）

- 整节点移入：指针同一性（`contiguous_payload() == 喂入存储+5`）、`payload.size()==1`、take 即时生效
- 跨节点保持原布局：payload 双节点/三节点/单字节节点链，`contiguous_payload()==nullptr`，
  drain（测试侧 span 游走）还原全部字节
- 部分头/部分体跨多次 feed 的 NeedMore→Ok；零长记录；版本 0x0301/02/03 透传
- Fatal：未知类型（24/0）、超长（18433）；边界 18432 恰好通过
- 所有权：记录活过 reader 析构（池先于记录声明）；reset 只清 pending 不动已取记录
- 连续/跨节点/连续混排

## 5. 记录层（二）：TlsRecordWriter 出向分片（2026-09-20 补）

### 交付物

| 文件 | 内容 |
|---|---|
| `include/fiber/tls/record/TlsRecordWriter.h` + `src/tls/record/TlsRecordWriter.cpp` | 出向分帧器（push 模型，take 语义） |
| `TlsRecord.h` 内 `tls_encode_record_header` | 5 字节头编码（`tls_decode_record_header` 的逆，inline） |
| `tests/TlsRecordWriterTest.cpp` | 12 用例（含 Reader 回环） |

### API

```cpp
TlsRecordWriter writer(node_pool);          // 或默认构造 + bind_node_pool()
writer.set_legacy_version(0x0301);          // 默认 0x0303；首飞客户端惯例 0x0301
mem::IoBufChain out(node_pool);             // 必须与 writer 同池绑定
writer.write(TlsContentType::Handshake, mem::IoBufChain &&payload, out);
// IoBuf&& 重载内部包一层链后同路径；返回 IoResult<void>，仅 NoMem
```

### 关键决策

- **take 语义对 Reader 镜像**：每条记录 `payload.take_prefix(chunk≤2^14, record_payload)`——
  整节点无 refcount 移出、边界节点 `retain_slice` 零拷贝视图。每记录唯一新分配 = 5 字节
  头 IoBuf + 头节点（append 的链节点）。
- **单池契约**：`IoBufChain` 无默认池（`node_pool()` 断言非空），故 writer 持池；
  `out` 与所有 payload 链必须同池（任何非空链天然已绑定）。池不匹配经 `append_chain`
  返回 false → `NoMem`（软失败），未绑定经 `FIBER_ASSERT`（编程错误）。这是 H3
  write_body node_pool mismatch 教训的直接应用。
- **空 payload 发一条零长记录**：write 至少产出一条记录（app-data keepalive 合法；
  FSM 在其他类型上避免传空）。
- **纯明文分帧**：AEAD/序号归 `TlsRecordCipher`（下一步）。多节点 payload 记录的
  seal 需要 cipher 侧 gather（每条记录至多一次拷贝），边界对齐时仍是纯零拷贝。
- **失败半成品**：NoMem 时 `out` 可能已带前缀分帧——调用方按连接级致命处理。

### 测试覆盖（12 绿，全量 2150 绿）

- 头编码 roundtrip（4 类型 × 3 版本 × 4 长度含 0/0x4000/0x4800）
- 小记录字节级精确（15 字节比对）；整节点移入指针同一性；版本可配置（0x0301 入头）
- 空 payload 单零长记录；恰 2^14 单记录；+1 字节分裂两条（内容拼接还原）
- 链输入：3 节点整移零拷贝（3 span 指针同一）；单缓冲 2^14+3616 边界视图零拷贝
  （首 span 指针 == 原存储）；双 10000 节点错位边界分裂
- 多次 write 追加连续记录（alert→appdata 类型/顺序）
- **Writer→Reader 回环**：2.5 条记录 3 节点 + 追加握手写 → 同池 Reader 全量还原
  （3 条 app + 1 条 handshake、内容逐字节、pending 清零）

## 6. 待做（记录层余下部分）

- ~~`record/TlsRecordCipher`：方向级保护状态（1.3 AEAD 静态 IV⊕seq / 1.2 GCM explicit nonce +
  序号回绕、KeyUpdate 换钥）；其入参即 `TlsRecord`——`contiguous_payload()` 走单 span 原地
  解密快路径，跨节点时 gather 后 open（每条记录至多一次拷贝）；Writer 侧 seal 后类型改写
  application_data + 长度域含 tag~~ **已完成（05 号，2026-09-20）**：span 化纯原语，
  epoch 由引擎换实例表达；链编排已从引擎义务收敛为 record/ 链适配层
  `TlsRecordCipherChain`（IoBufChain 输入的转录/原地 × seal/open 四形态，跨节点自动
  退化转录、原地 seal 前后缀外置经 seal_scatter——引擎只剩调用，见 05 §13）
- 明文记录（握手初期）与密文记录的路由由引擎层（TlsEngine）做，Reader/Writer 不区分
