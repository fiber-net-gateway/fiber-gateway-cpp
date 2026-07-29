# `src/dns/` 模块审计

> 审计范围：`src/dns/` 全部 16 个文件（约 4500 行），按传输与报文编解码（`DnsClient`/`DnsName`/`DnsMessage`）、缓存（`DnsCache`/`SharedDnsCache`）、解析器（`DnsResolverLocal`/`DnsResolver`）三个域并行评审后整合。所有结论均经直接读码 + 交叉验证。
>
> 后续状态：旧 `DnsCache`/`SharedDnsCache` 与 `NameSnapshot` 已由 `DnsCache2`/`SharedDnsCache2` 和固定容量 `DnsAddressSet` 替代；下文涉及旧缓存实现的行号与结论保留为历史审计记录。
>
> 核验事实：`RWMutex::ReadLockAwaiter::await_ready` 在无写锁竞争时 `try_lock_shared()` 返回 true、`co_await lock_shared()` 不挂起（`src/async/RWMutex.cpp:153-162,200-207`）；`DnsClient` 的 UDP 接收由 `RWFd` read callback 驱动，回调恢复查询协程后通过栈上 dispatch invalidation observer 判断 client 是否已被同步 `close()`/`release()`/析构，失效后不再访问 client；open-addressing 的 tombstone/探查不变量正确；`NameSnapshot` 自包含拷贝、无悬空指针。
>
> 状态（2026-07-13 复核）：HIGH #1、#2、#3 及 MEDIUM #4、#5 均已修复；MEDIUM #6 已确认是符合 NXDOMAIN 语义的有意行为；LOW #1、#13 已随对应 HIGH 项修复，其余排期。
>
> 复核验证：`cmake --build build --target fiber_tests -j2`；DNS 定向测试 44/44 通过；`ctest --test-dir build --output-on-failure`，1147/1147 通过。

## 总体评价

wire codec（`DnsName`/`DnsMessage`）边界检查、指针压缩循环检测（强制向后指 + 跳数上界）、open-addressing 哈希不变量、SOA 负 TTL 解析（RFC 2308）、pending 合并等均正确。原始审计发现的三个高危问题及缓存 LRU/tombstone 退化均已修复；当前剩余工作主要是分配开销及若干合规问题。按严重度排列如下，均给出 `file:line` 与触发场景。

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
- slot 保存无分配的 `cancel_context + InflightCancelFn`，TCP connect 阶段取消 timeout awaiter，连接建立后取消 `TcpStream`（`DnsClient.h:58-72`，`DnsClient.cpp:290-313`）。
- `cancel_all_inflight` 先收集 cancel/resume 动作、清空 slot 内指针，再执行取消，避免同步重入破坏遍历（`DnsClient.cpp:458-499`）。
- `CloseAndReleaseCancelPendingTcpFallback` 覆盖 fallback 挂起时 `close()+release()`，验证查询被取消且对端连接关闭（`DnsClientTest.cpp:888-915`）。

### 3. ✅ 已修复：可预测的顺序事务 ID + 无 0x20 + 客户端不校验响应 question
原始位置：`DnsClient.cpp:130-133,500-507`（`next_id_++` 从 0 线性递增）/ `593-620`（`handle_udp_packet` 仅按 ID+源地址匹配）/ `DnsMessage.cpp:223-228`、`DnsName.cpp:42-44`（查询名原样编码，无 0x20）

**原始问题（修复前）**：16 位顺序 ID 易于预测（RFC 5452），查询名没有 0x20 大小写随机化。客户端 `handle_udp_packet` 在解析层校验前就把来源和 ID 匹配的包 memcpy 进 slot。默认本地端口为 0、由内核选择临时端口，且客户端会校验响应源 IP/端口，这提供了额外熵，但 socket 在整个 `DnsClient` 生命周期内复用，不能替代随机事务 ID 和完整 question 匹配。

修复前主解析路径已有两层缓解：`DnsResolverLocal::handle_response` 校验 question 的规范化 name/type/class（`DnsResolverLocal.cpp:732-747`），并且 HIGH #1 的修复已拒绝不属于 qname/CNAME 可达链的 answer。因此旧审计中“可直接借此触发 HIGH #1 并污染缓存”的链路已经断开。不过当时仍存在：
- `DnsClient::query_raw` 的直接调用者仍会接受 question 不匹配的包；
- 伪造包若先到达，`DnsResolverLocal` 会收到它并返回 `Invalid`，合法响应随后因 slot 已完成而被丢弃，可形成查询级 DoS；
- 若攻击者猜中源端口和顺序 ID，并伪造正确 question 与合法 answer 链，当前非 DNSSEC 校验路径仍可能接受响应。

`dnssec_ok` 只控制 EDNS DO bit，开启它也不等于执行 DNSSEC 验证，不能作为本问题的修复。

**当前实现**：
- `prepare_request` 每个 attempt 只调用一次 BoringSSL `RAND_bytes`，前 32 bit 生成随机 ID 起点和奇数探查步长，后 256 bit 原地随机化 QNAME ASCII 字母；失败时返回 `Unknown`，不会回退到顺序 ID（`DnsClient.cpp:418-448`）。
- `next_id_` 已删除；`select_query_id` 的奇数步长可遍历完整 65536 ID 空间，并继续使用 `id_to_slot_` 保证 active ID 唯一（`DnsQuerySecurity.cpp:66-80`）。
- `DnsClient::Options::enable_0x20` 默认开启；关闭时不随机化 QNAME，并使用 ASCII 大小写不敏感匹配（`DnsClient.h:27-36`，`DnsQuerySecurity.cpp:83-120`）。
- 无分配 `response_matches_query` 要求 ID、QR、QUERY opcode、QDCOUNT、QNAME、QTYPE、QCLASS 全部匹配，名称解码只使用两个固定 255-byte 栈数组（`DnsQuerySecurity.cpp:122-152`）。
- UDP 在 response capacity 检查、`memcpy` 和 `complete_slot` 前执行 matcher；不匹配或畸形报文被静默丢弃，slot 继续等待合法响应（`DnsClient.cpp:617-645`）。TCP 读取完整响应后调用同一 matcher，不匹配返回 `Invalid`，同时修复 LOW #13（`DnsClient.cpp:367-392`）。
- 随机缓冲位于普通非协程 `prepare_request` 栈上，slot 未增加字段或域名副本；生产热路径未新增堆分配。
- 回归测试覆盖 ID 碰撞/环回/完整空间探查、0x20 原地变换、错误 ID/qname/qtype/qclass/大小写、关闭严格 0x20、TCP 错误 ID/question，以及原有 fallback/cancel 行为（`DnsQuerySecurityTest.cpp`，`DnsClientTest.cpp:737-872`）。

---

## 🟠 MEDIUM

### 4. ✅ 已修复：`SharedDnsCache::lookup_name` 的 LRU 更新丢失及过期条目不清理
`DnsCache.cpp:994-1013`(lookup 用 peek + try_lock touch) / `581-602`(peek 不调 cleanup) / `639-656`(note_name_access 触摸已过期条目)

读锁下 `peek_name`（const，不清理、不 touch），再 `try_lock` 升级写锁做 `note_name_access`。upsert 繁忙时 `try_lock` 持续失败 -> LRU 永不更新，退化为 FIFO，热条目被先淘汰、命中率崩溃。`peek` 不调 `cleanup_entry`，过期条目只靠后台 `sweep_expired` 回收。`note_name_access` 还会给已过期条目续 `access_clock_`，污染淘汰决策。

**修复**：`access_clock_`/`approx_last_access` 改 atomic，读锁下直接 touch（近似 LRU 允许 lost update）；或读锁下做幂等的 cleanup+touch；提高 sweep 频率。

**当前实现**：
- `access_clock_` 和每个 entry 的 `approx_last_access` 使用 relaxed atomic；共享读锁内确认快照命中后直接 touch，不再使用 `try_lock` 升级写锁。
- 共享查询区分真正 Miss 与全部 slot 已过期的 entry；后者释放读锁后获取写锁并重新 lookup，在锁内复核并删除，普通 hit/miss 不增加写锁竞争。
- `sweep_expired` 的 budget 改为扫描预算，避免小 budget 仍扫描完整缓存；共享写路径在已有写锁下顺带执行固定小预算增量 sweep。
- 回归测试覆盖共享查询回收过期 entry 及共享命中刷新淘汰年龄。

### 5. ✅ 已修复：`DnsCache` tombstone 累积、无 rehash
`DnsCache.cpp:377-391`(erase 置 tombstone，仅靠插入复用)

短 TTL + sweep 频繁 erase 产生大量 tombstone，无回收/重哈希机制，探查链逐步退化到接近 O(bucket_count)。

**修复**：统计 tombstone 密度，超阈值重哈希压缩；或采用 backward-shift 删除。

**当前实现**：
- 维护 `tombstone_count_`，删除时增加、插入复用时减少；达到 bucket 数量 1/4 时，在现有 bucket 数组内清空并重新索引所有 occupied entry，不产生新分配。
- 显式配置的 `index_capacity` 也强制不低于 `2 * max_entries`，将最大装载率限制为 0.5。
- lookup 过期删除、显式 erase、容量淘汰和 sweep 后都会在安全边界检查重建阈值；回归测试覆盖反复 erase/reinsert 后的索引可用性。

### 6. ✅ 已确认非缺陷：`upsert_negative_nxdomain` 有意替换同名所有 RRset
`DnsCache.cpp:847-850`（对比 `699`/`786`/`909` 都先 load）

其它 upsert 都先 `load_entry_state` 保留同条目其它类型记录，唯独 NxDomain 不 load，会清掉同名未过期的 A/AAAA/CNAME。NxDomain 语义上"名字不存在"清空也说得通（RFC），但不一致更像遗漏。

**结论**：NXDOMAIN 按 `<QNAME,QCLASS>` 表示名字不存在，而 NODATA 才按 `<QNAME,QTYPE,QCLASS>` 表示特定 RRset 不存在。保留 A/AAAA/CNAME 会使单个快照同时包含 positive/CNAME 和 NXDOMAIN；当前清空同名 RRset 的行为是合理策略。实现已补充意图注释，并新增 positive/CNAME -> NXDOMAIN 以及 NXDOMAIN -> positive 的双向回归测试，不添加 `load_entry_state`。

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
13. **✅ 已修复：`query_tcp` 未校验响应 ID**。UDP/TCP 现共用 `response_matches_query`，校验 ID 和完整 question；TCP 不匹配返回 `Invalid`（`DnsClient.cpp:367-392`）。
14. **✅ 已修复：`recv_loop` 对非 Canceled/BadFd 错误不 yield**。独立接收协程已删除；持久 read callback 每次就绪使用非阻塞 `try_recv_from` 排空 socket，遇到任意读取错误即返回 EventLoop，不会在单次 dispatch 内忙等。

---

## 修复优先级

| 优先级 | 状态 | 条目 | 工作量 |
|--------|------|------|--------|
| P0 | ✅ 已修复 | HIGH #1（pending 完成顺序 + 防重复上游查询 + answer/CNAME 链校验） | - |
| P0 | ✅ 已修复 | HIGH #2（TCP fallback 取消） | - |
| P0 | ✅ 已修复 | HIGH #3（随机 ID + 0x20 + 客户端 UDP/TCP question 校验） | - |
| P1 | ✅ 已修复/确认 | MEDIUM #4/#5/#6（缓存 LRU/清理/tombstone/NXDOMAIN 语义） | - |
| P1 | ⏳ 待修复 | MEDIUM #7（堆分配 churn） | 中 |
| P2 | ⏳ 待修复 | MEDIUM #8 + 其余未修复 LOW | 排期 |

## 已验证为正确的部分

- `decode_name` 指针压缩：强制 `target < pos`（向后指）+ `jumps > packet_len` 循环断路（`DnsName.cpp:124,132`）。
- `encode_name` 标签长度 1..63 校验、空标签拒绝、根处理（`DnsName.cpp:35-58`）。
- `MessageParser::parse` 记录总数 uint32 累加无溢出、与 max_records 比较（`DnsMessage.cpp:157-162`）。
- `encode_query` EDNS OPT 记录格式正确（root 名、type=OPT、class=payload size、TTL=version+DO、RDLENGTH=0）（`DnsMessage.cpp:244-253`）。
- `DnsCache` open-addressing tombstone 语义：find 跨过 tombstone、insert 复用首个 tombstone、erase 置 tombstone 保可达性；tombstone 达阈值后原地重建索引压缩探查链。
- `ensure_capacity` 字节记账：`bytes_used_ - old_blob_size + new_blob_size`，old 来自 entry 当前 blob_size，无下溢（`DnsCache.cpp:441,486,514`）。
- blob 对齐：地址记录区 `align_up(., alignof(IpAddress))`，`new char[]` 默认对齐 >= alignof(IpAddress)（`DnsCache.cpp:472,491`）。
- `handle_response` 校验响应 question 段匹配 qname/qtype/qclass（`DnsResolverLocal.cpp:732-747`）。
- SOA 负 TTL = `min(record.ttl, minimum)`（RFC 2308）（`DnsResolverLocal.cpp:106`）。
- literal IP 直返不查 DNS、按 policy 过滤（`DnsResolver.cpp:266-281`）。
- `DnsClient` UDP read callback 恢复查询协程后，先检查栈上 dispatch invalidation observer；若恢复路径同步关闭、释放或析构 client，则立即停止本轮排空，不再读取 client 成员。
