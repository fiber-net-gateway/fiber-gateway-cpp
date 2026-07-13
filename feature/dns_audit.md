# `src/dns/` 模块审计

> 审计范围：`src/dns/` 全部 14 个文件（约 4300 行），按传输与报文编解码（`DnsClient`/`DnsName`/`DnsMessage`）、缓存（`DnsCache`/`SharedDnsCache`）、解析器（`DnsResolverLocal`/`DnsResolver`）三个域并行评审后整合。所有结论均经直接读码 + 交叉验证。
>
> 核验事实：`RWMutex::ReadLockAwaiter::await_ready` 在无写锁竞争时 `try_lock_shared()` 返回 true、`co_await lock_shared()` 不挂起（`src/async/RWMutex.cpp:153-162,200-207`）；`RWFd::close()` 同步 `resume()` 挂起的读等待者并置 `Canceled`（`src/net/detail/RWFd.cpp:38-71`），故 `~DnsClient`->`close()`->`socket_->close()` 会在析构成员前同步排空 `recv_loop`，recv_loop 无 UAF；open-addressing 的 tombstone/探查不变量正确；`NameSnapshot` 自包含拷贝、无悬空指针。
>
> 状态（2026-07-13 复核）：HIGH #1、#2 已修复；HIGH #3 尚未修复，但主解析路径已有 question/answer 语义校验缓解；LOW #1 随 HIGH #1 一并修复，其余排期。
>
> 复核验证：`cmake --build build --target fiber_tests -j2`；`./build/fiber_tests --gtest_filter='DnsClientTest.*:DnsResolverLocalTest.*'`，12/12 通过。

## 总体评价

wire codec（`DnsName`/`DnsMessage`）边界检查、指针压缩循环检测（强制向后指 + 跳数上界）、open-addressing 哈希不变量、SOA 负 TTL 解析（RFC 2308）、pending 合并等均正确。原始审计发现的两个内存/活性高危问题（HIGH #1/#2）已经修复；当前仍需优先处理 HIGH #3 的 DNS 响应匹配与随机化加固，以及若干中危的并发/性能/合规问题。按严重度排列如下，均给出 `file:line` 与触发场景。

---

## 🔴 HIGH

### 1. ✅ 已修复：畸形/伪造响应下无限重查上游 + 跟随者协程被孤儿化（永久挂起）
原始位置：`DnsResolverLocal.cpp:523`(循环) / `537`(仅 CNAME 分支检查 hop) / `576-577`(complete 在 release 之前) / `628`(只校验 question 不校验 answer owner)

**原始问题（修复前）**：

两个问题同根，触发条件一致：响应的 **answer 记录 owner ≠ qname 且无 qname 的 CNAME**（例如伪造一个 ID+question 都对、但 answer 是别的名字的 A 记录）。`handle_response` 会把这些"别人家"的记录 upsert 进缓存（`cache_updated=true`、`retry_from_cache=true`、`has_status=false`），然后：

- **无限循环**：`for(hop=0;;++hop)` 只在 CNAME 缓存命中分支（`537`）检查 `hop`，上游重试路径直接 fall-through `continue`，无任何迭代上限。leader 对 qname 的缓存查找持续 Miss -> 反复 `query_upstream`，每次都打真实上游。攻击者可借此放大 DNS 流量。
- **孤儿化**：leader 在 `576` 调 `complete_pending`（**内联 resume** 所有跟随者），`577` 才调 `release_pending`。被 resume 的跟随者重入循环做 `co_await cache_->lookup_name`--已验证 `RWMutex::ReadLockAwaiter::await_ready` 在无写锁竞争时 `try_lock_shared()` 返回 true（`RWMutex.cpp:153-162,200-207`），**不挂起**。若该 lookup Miss（畸形响应场景），跟随者同步跑到 `find_pending`，发现条目仍 `active`（release 还没调），重新入队并挂起；随后 leader `release_pending` 把 `waiters=nullptr`、`active=false` --跟随者**永远不会再被唤醒**，协程泄漏，其调用方 `co_await resolve()` 永久挂起。

注意 `handle_response` 只校验 question 段（`628`），不校验 answer 的 owner，所以伪造包能通过。`retry_from_cache` 字段虽被置位但从未被读取（见 LOW #1），重试实为 `has_status=false` 的隐式副作用。

**原修复建议**：
- (a) 循环加迭代硬上限：`for(hop=0; hop<max_cname_hops; ++hop)` 或独立 cap；
- (b) 先保存 waiter 链表并 `entry.active=false` 再 resume，或 `release_pending` 先于 `complete_pending`；
- (c) 顺带校验 answer owner ∈ {qname, CNAME 链}。

**当前实现（commit `7b739ca`）**：
- `finish_pending` 先摘下 waiter 链并 `release_pending`，再逐个 `resume`，重入时旧 pending 已不再 active（`DnsResolverLocal.cpp:520-540`）。
- `PendingOutcome::action` 显式区分 `ReturnStatus`/`RetryFromCache`；上游返回要求缓存命中后，通过 `expect_cache_hit` 保证 Miss 立即返回 `Invalid`，不会再次查询上游（`DnsResolverLocal.h:106-115`，`DnsResolverLocal.cpp:631-705`）。
- `inspect_answer_set` 只采纳从 qname 沿唯一 CNAME 链可达的 A/AAAA，忽略混合响应中的无关记录；若响应只有无关 answer 则拒绝，并同时拒绝冲突 CNAME 和超限链（`DnsResolverLocal.cpp:137-225,772-904`）。
- 回归测试覆盖无关 answer、跟随者全部完成、只发一次上游查询且不污染缓存（`DnsResolverLocalTest.cpp:564-615`）。

### 2. ✅ 已修复：TCP fallback 未被 `close()` 取消 -> 析构期 use-after-free
原始位置：`DnsClient.cpp:251-254`(query_tcp) / `403-429`(cancel_all_inflight) / `285`(query_tcp 持 `const InflightSlot&`)

**原始问题（修复前）**：

`query_tcp` 持有指向 `slots_[i]` 的引用并在 TCP I/O 上挂起。`close()`->`cancel_all_inflight` 只 resume `slot.waiter` 非空的等待者；TCP fallback 期间 `slot.waiter` 为空（coroutine 挂在 `query_tcp` 内的 `stream.write/read` 上），**不会被唤醒**。若此时 `~DnsClient`/`release()` 释放 `slots_`，`query_tcp` 恢复后访问悬空引用 -> UAF/崩溃。`close()` 名义上"取消所有 inflight"却漏掉了 TCP fallback，对调用方有误导。

**原修复建议**：把 in-flight `TcpStream`（或取消令牌）记入 slot，在 `cancel_all_inflight` 里 close 它，使 `query_tcp` 立即以错误返回；或在 `query_tcp` 每个挂起点后检查 `closing_` 并要求调用方 await 静默。

**当前实现（commit `2e44ea1`）**：
- slot 保存无分配的 `cancel_context + InflightCancelFn`，TCP connect 阶段取消 timeout awaiter，连接建立后取消 `TcpStream`（`DnsClient.h:55-71`，`DnsClient.cpp:297-318`）。
- `cancel_all_inflight` 先收集 cancel/resume 动作、清空 slot 内指针，再执行取消，避免同步重入破坏遍历（`DnsClient.cpp:428-469`）。
- `CloseAndReleaseCancelPendingTcpFallback` 覆盖 fallback 挂起时 `close()+release()`，验证查询被取消且对端连接关闭（`DnsClientTest.cpp:631-658`）。

### 3. ⏳ 未修复（主路径部分缓解）：可预测的顺序事务 ID + 无 0x20 + 客户端不校验响应 question
`DnsClient.cpp:130-133,500-507`（`next_id_++` 从 0 线性递增）/ `593-620`（`handle_udp_packet` 仅按 ID+源地址匹配）/ `DnsMessage.cpp:223-228`、`DnsName.cpp:42-44`（查询名原样编码，无 0x20）

16 位顺序 ID 易于预测（RFC 5452），查询名没有 0x20 大小写随机化。客户端 `handle_udp_packet` 在解析层校验前就把来源和 ID 匹配的包 memcpy 进 slot。默认本地端口为 0、由内核选择临时端口，且客户端会校验响应源 IP/端口，这提供了额外熵，但 socket 在整个 `DnsClient` 生命周期内复用，不能替代随机事务 ID 和完整 question 匹配。

当前主解析路径有两层缓解：`DnsResolverLocal::handle_response` 校验 question 的规范化 name/type/class（`DnsResolverLocal.cpp:732-747`），并且 HIGH #1 的修复已拒绝不属于 qname/CNAME 可达链的 answer。因此旧审计中“可直接借此触发 HIGH #1 并污染缓存”的链路已经断开。不过：
- `DnsClient::query_raw` 的直接调用者仍会接受 question 不匹配的包；
- 伪造包若先到达，`DnsResolverLocal` 会收到它并返回 `Invalid`，合法响应随后因 slot 已完成而被丢弃，可形成查询级 DoS；
- 若攻击者猜中源端口和顺序 ID，并伪造正确 question 与合法 answer 链，当前非 DNSSEC 校验路径仍可能接受响应。

`dnssec_ok` 只控制 EDNS DO bit，开启它也不等于执行 DNSSEC 验证，不能作为本问题的修复。

#### 代码修复方案

1. **每次分配事务 ID 都使用 CSPRNG（必须）**
   - 在 `DnsClient.cpp` 使用项目已经链接的 BoringSSL `RAND_bytes`，不使用 `std::random_device`、`std::mt19937`，也不新增堆分配。
   - `allocate_query_id()` 每次读取 32 位随机数：低 16 位作为随机起点，高 16 位强制置奇数后作为探查步长；按 `candidate = start + i * odd_step` 在 16 位空间探查。奇数步长可遍历完整 65536 个 ID，既保证首选 ID 不可预测，也能在高占用时找到任意空闲 ID。
   - `RAND_bytes` 失败时 fail closed，返回错误，不回退到可预测的顺序 ID。删除 `next_id_` 成员以及 `init/reset` 中对它的初始化。

2. **在发送缓冲区内原地实现 0x20（建议默认开启、允许显式关闭）**
   - 给 `DnsClient::Options` 增加 `enable_0x20 = true`。`encode_query` 完成后，遍历从 DNS header 后开始的未压缩 QNAME，仅对 ASCII 字母用 CSPRNG bit 随机选择大小写；label 长度、QTYPE/QCLASS 和 name 长度保持不变。
   - 最多 253 个名称字节，只需固定大小栈缓冲保存随机 bit；不得引入 `std::string`/`std::vector`。发出的随机大小写 query 已保存在 slot 的 `request_buf` 中，可直接作为后续精确匹配基准。
   - 关闭 0x20 时，question 名按 DNS ASCII 大小写不敏感规则比较；开启时必须逐字符精确比较大小写，否则 0x20 不提供额外熵。

3. **`DnsClient` 在完成 slot 前校验完整 question（必须）**
   - 增加无分配 helper，例如 `response_matches_request(const InflightSlot&, const uint8_t*, size_t)`：要求 QR=1、opcode=QUERY、QDCOUNT=1，使用现有 `decode_name()` 将 request/response QNAME 解码到两个固定栈数组，再比较 name、QTYPE、QCLASS。
   - UDP 中应在 `memcpy`/`complete_slot` 之前调用。来源或 ID 匹配但 question 不匹配/报文畸形时只丢弃该包，继续等待合法响应；不能用错误结束 slot，否则攻击者仍可制造提前失败。
   - 保留现有源 IP/端口和 ID 检查。question 校验放在 response-capacity 错误之前，避免一个 question 不匹配的大包把合法查询提前完成为 `NoMem`。

4. **UDP/TCP 共用同一响应匹配 helper（必须）**
   - `query_tcp` 读完 length-prefixed 响应后，同样校验响应 ID 和完整 question；失败返回 `Invalid`。这会同时修复 LOW #13。
   - TCP 是一问一连接，匹配失败可以结束查询；UDP 则必须忽略不匹配包并继续等待。

5. **回归测试**
   - UDP server 先发送“同源、同 ID、错误 qname/qtype/qclass”的响应，再发送正确响应；断言客户端忽略前者并返回后者。
   - server 读取实际随机化后的 QNAME，先返回一个翻转大小写的 question，再精确回显；开启 0x20 时只接受精确回显。
   - 并发查询断言所有 active ID 唯一；对 ID 生成器用可注入的固定随机字节或独立小组件测试碰撞与完整空间探查，避免用概率断言测试“看起来随机”。
   - TCP 分别覆盖错误 ID、错误 question，并保留现有 truncated fallback 与 close/cancel 测试。

推荐落地顺序：先做“随机 ID + 客户端 question 校验 + TCP 共用校验”，这是安全边界；随后启用 0x20。两步均完成后再将本项标记为已修复。

---

## 🟠 MEDIUM

### 4. `SharedDnsCache::lookup_name`：peek + try_lock，写竞争下丢失 LRU 更新且不清理过期
`DnsCache.cpp:994-1013`(lookup 用 peek + try_lock touch) / `581-602`(peek 不调 cleanup) / `639-656`(note_name_access 触摸已过期条目)

读锁下 `peek_name`（const，不清理、不 touch），再 `try_lock` 升级写锁做 `note_name_access`。upsert 繁忙时 `try_lock` 持续失败 -> LRU 永不更新，退化为 FIFO，热条目被先淘汰、命中率崩溃。`peek` 不调 `cleanup_entry`，过期条目只靠后台 `sweep_expired` 回收。`note_name_access` 还会给已过期条目续 `access_clock_`，污染淘汰决策。

**修复**：`access_clock_`/`approx_last_access` 改 atomic，读锁下直接 touch（近似 LRU 允许 lost update）；或读锁下做幂等的 cleanup+touch；提高 sweep 频率。

### 5. `DnsCache` tombstone 累积、无 rehash
`DnsCache.cpp:377-391`(erase 置 tombstone，仅靠插入复用)

短 TTL + sweep 频繁 erase 产生大量 tombstone，无回收/重哈希机制，探查链逐步退化到接近 O(bucket_count)。

**修复**：统计 tombstone 密度，超阈值重哈希压缩；或采用 backward-shift 删除。

### 6. `upsert_negative_nxdomain` 未 `load_entry_state`，与其它 upsert 不一致
`DnsCache.cpp:847-850`（对比 `699`/`786`/`909` 都先 load）

其它 upsert 都先 `load_entry_state` 保留同条目其它类型记录，唯独 NxDomain 不 load，会清掉同名未过期的 A/AAAA/CNAME。NxDomain 语义上"名字不存在"清空也说得通（RFC），但不一致更像遗漏。

**修复**：为一致性加上 `load_entry_state`；若刻意清空，加注释说明。

### 7. 每次 resolve 的堆分配 churn
`DnsResolverLocal.cpp:659`(每响应 `make_unique<IpAddress[]>`) / `DnsResolver.cpp:326-327,389-400,495-498`(FamilyQueryState×2 + merged + AddressResolveResult)

单次 resolve 热路径 3–5 次 malloc，与本仓库"最小化分配 churn"原则相悖。`temp_records` 数量受 `max_records`(默认 16) 上界约束，完全可上栈。

**修复**：`temp_records` 改 `std::array<IpAddress, N>` 上栈；复用 `ResolveResult`（reset 而非 destroy/re-init）；用 `BufPool`。

### 8. 双族解析总是并行查 A+AAAA，无 happy-eyeballs racing
`DnsResolver.cpp:373-377`(V6First/V4First 都 spawn 两个 + WaitGroup 等齐)

V6First 也会同时发 A 查询并等齐两者才合并，上游查询量翻倍。对网关场景可接受，但浪费。

**修复**：V6First 先发 AAAA，失败/超时再回退 A；或连接级 racing。

---

## 🟡 LOW

1. **✅ 已修复：`retry_from_cache` 是死字段**。已替换为会被 leader/follower 显式读取的 `PendingAction::RetryFromCache`，并配合 `expect_cache_hit` 防止隐式重查（`DnsResolverLocal.h:106-115`，`DnsResolverLocal.cpp:631-705`）。
2. **`find_pending` O(max_pending) 线性扫描**：`DnsResolverLocal.cpp:339-355`。高 QPS 下值得加 (name,type,class) 哈希索引。
3. **answer 去重 O(n²) 且重复 normalize**：`DnsResolverLocal.cpp:696-714,724-737,751-773`。预 normalize 一次。
4. **无 SOA 时不缓存负响应**：`DnsResolverLocal.cpp:642-656,796-812`。RFC 2308 建议默认 TTL，否则不存在域名反复打上游。
5. **OPT 扩展 RCODE 未提取**：`DnsMessage.cpp` 把 OPT 当普通记录。既然有 `dnssec_ok`，DNSSEC 场景下扩展 RCODE 会丢。
6. **FNV-1a 无 avalanche，同后缀名聚集**：`DnsCache.cpp:221-231`。加 finalizer 或换 wyhash。
7. **`MessageParser` name_storage 默认 2048 偏小**：`DnsMessage.h:52`。多条长名响应返回 NoMem（优雅失败）。
8. **三类 Result 类型大量重复**（`ResolveResult`/`AddressResolveResult`/`EndpointResolveResult`，~200 行样板，canonical_name 层层拷贝）。可合并/模板化/移动语义。
9. **`store_entry_state` 空 owner 且 is_new 时泄漏条目**：`DnsCache.cpp:454-460`。当前 `normalize_name` 不产空串故不可达，防御性加 `recycle_entry`。
10. **`ensure_capacity` 两次估算（粗->精）**：`DnsCache.cpp:683` vs `486`。先算完整 blob size 再调。
11. **`NameSnapshot` 同时 NonCopyable+NonMovable**：`DnsCache.h:20`。强制按出参传递或堆间接，去掉 NonMovable 即可移动。
12. **`pick_status_family` 双失败时返回更严重的状态**：`DnsResolver.cpp:72-86`。NxDomain(确定) vs ServerFailure(瞬时)，返回 NxDomain 更利于调用方缓存负结果。
13. **`query_tcp` 未校验响应 ID**：TCP 响应未比对事务 ID，仅靠连接匹配。RFC 合规缺口。
14. **`recv_loop` 对非 Canceled/BadFd 错误不 yield**：`DnsClient.cpp:274-279`。UDP ICMP 错误单次会被 recvmsg 消费，但错误洪流下可能短时忙等。

---

## 修复优先级

| 优先级 | 状态 | 条目 | 工作量 |
|--------|------|------|--------|
| P0 | ✅ 已修复 | HIGH #1（pending 完成顺序 + 防重复上游查询 + answer/CNAME 链校验） | - |
| P0 | ✅ 已修复 | HIGH #2（TCP fallback 取消） | - |
| P0 | ⏳ 待修复 | HIGH #3（随机 ID + 0x20 + 客户端 UDP/TCP question 校验） | 中 |
| P1 | ⏳ 待修复 | MEDIUM #4/#5/#6（缓存 LRU/清理/tombstone/NxDomain 一致性） | 中 |
| P1 | ⏳ 待修复 | MEDIUM #7（堆分配 churn） | 中 |
| P2 | ⏳ 待修复 | MEDIUM #8 + 其余未修复 LOW | 排期 |

## 已验证为正确的部分

- `decode_name` 指针压缩：强制 `target < pos`（向后指）+ `jumps > packet_len` 循环断路（`DnsName.cpp:124,132`）。
- `encode_name` 标签长度 1..63 校验、空标签拒绝、根处理（`DnsName.cpp:35-58`）。
- `MessageParser::parse` 记录总数 uint32 累加无溢出、与 max_records 比较（`DnsMessage.cpp:157-162`）。
- `encode_query` EDNS OPT 记录格式正确（root 名、type=OPT、class=payload size、TTL=version+DO、RDLENGTH=0）（`DnsMessage.cpp:244-253`）。
- `DnsCache` open-addressing tombstone 语义：find 跨过 tombstone、insert 复用首个 tombstone、erase 置 tombstone 保可达性（`DnsCache.cpp:255-301,377-391`）。
- `ensure_capacity` 字节记账：`bytes_used_ - old_blob_size + new_blob_size`，old 来自 entry 当前 blob_size，无下溢（`DnsCache.cpp:441,486,514`）。
- blob 对齐：地址记录区 `align_up(., alignof(IpAddress))`，`new char[]` 默认对齐 >= alignof(IpAddress)（`DnsCache.cpp:472,491`）。
- `handle_response` 校验响应 question 段匹配 qname/qtype/qclass（`DnsResolverLocal.cpp:732-747`）。
- SOA 负 TTL = `min(record.ttl, minimum)`（RFC 2308）（`DnsResolverLocal.cpp:106`）。
- literal IP 直返不查 DNS、按 policy 过滤（`DnsResolver.cpp:266-281`）。
- `RWFd::close()` 同步 resume 读等待者，`recv_loop` 在析构前被排空，无 UAF（`RWFd.cpp:38-71`）。
