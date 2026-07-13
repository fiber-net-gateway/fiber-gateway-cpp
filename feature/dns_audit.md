# `src/dns/` 模块审计

> 审计范围：`src/dns/` 全部 14 个文件（约 4300 行），按传输与报文编解码（`DnsClient`/`DnsName`/`DnsMessage`）、缓存（`DnsCache`/`SharedDnsCache`）、解析器（`DnsResolverLocal`/`DnsResolver`）三个域并行评审后整合。所有结论均经直接读码 + 交叉验证。
>
> 核验事实：`RWMutex::ReadLockAwaiter::await_ready` 在无写锁竞争时 `try_lock_shared()` 返回 true、`co_await lock_shared()` 不挂起（`src/async/RWMutex.cpp:153-162,200-207`）；`RWFd::close()` 同步 `resume()` 挂起的读等待者并置 `Canceled`（`src/net/detail/RWFd.cpp:38-71`），故 `~DnsClient`->`close()`->`socket_->close()` 会在析构成员前同步排空 `recv_loop`，recv_loop 无 UAF；open-addressing 的 tombstone/探查不变量正确；`NameSnapshot` 自包含拷贝、无悬空指针。
>
> 状态：未动工。优先修 HIGH #1/#2/#3，其余排期。

## 总体评价

wire codec（`DnsName`/`DnsMessage`）边界检查、指针压缩循环检测（强制向后指 + 跳数上界）、open-addressing 哈希不变量、SOA 负 TTL 解析（RFC 2308）、pending 合并等均正确。存在 **2 个高危**（一个可致调用方永久挂起 + 协程泄漏，一个析构期 UAF）、若干中危的并发/性能/合规问题。按严重度排列如下，均给出 `file:line` 与触发场景。

---

## 🔴 HIGH

### 1. 畸形/伪造响应下：无限重查上游 + 跟随者协程被孤儿化（永久挂起）
`DnsResolverLocal.cpp:523`(循环) / `537`(仅 CNAME 分支检查 hop) / `576-577`(complete 在 release 之前) / `628`(只校验 question 不校验 answer owner)

两个问题同根，触发条件一致：响应的 **answer 记录 owner ≠ qname 且无 qname 的 CNAME**（例如伪造一个 ID+question 都对、但 answer 是别的名字的 A 记录）。`handle_response` 会把这些"别人家"的记录 upsert 进缓存（`cache_updated=true`、`retry_from_cache=true`、`has_status=false`），然后：

- **无限循环**：`for(hop=0;;++hop)` 只在 CNAME 缓存命中分支（`537`）检查 `hop`，上游重试路径直接 fall-through `continue`，无任何迭代上限。leader 对 qname 的缓存查找持续 Miss -> 反复 `query_upstream`，每次都打真实上游。攻击者可借此放大 DNS 流量。
- **孤儿化**：leader 在 `576` 调 `complete_pending`（**内联 resume** 所有跟随者），`577` 才调 `release_pending`。被 resume 的跟随者重入循环做 `co_await cache_->lookup_name`--已验证 `RWMutex::ReadLockAwaiter::await_ready` 在无写锁竞争时 `try_lock_shared()` 返回 true（`RWMutex.cpp:153-162,200-207`），**不挂起**。若该 lookup Miss（畸形响应场景），跟随者同步跑到 `find_pending`，发现条目仍 `active`（release 还没调），重新入队并挂起；随后 leader `release_pending` 把 `waiters=nullptr`、`active=false` --跟随者**永远不会再被唤醒**，协程泄漏，其调用方 `co_await resolve()` 永久挂起。

注意 `handle_response` 只校验 question 段（`628`），不校验 answer 的 owner，所以伪造包能通过。`retry_from_cache` 字段虽被置位但从未被读取（见 LOW #1），重试实为 `has_status=false` 的隐式副作用。

**修复**：
- (a) 循环加迭代硬上限：`for(hop=0; hop<max_cname_hops; ++hop)` 或独立 cap；
- (b) 先保存 waiter 链表并 `entry.active=false` 再 resume，或 `release_pending` 先于 `complete_pending`；
- (c) 顺带校验 answer owner ∈ {qname, CNAME 链}。

### 2. TCP fallback 未被 `close()` 取消 -> 析构期 use-after-free
`DnsClient.cpp:251-254`(query_tcp) / `403-429`(cancel_all_inflight) / `285`(query_tcp 持 `const InflightSlot&`)

`query_tcp` 持有指向 `slots_[i]` 的引用并在 TCP I/O 上挂起。`close()`->`cancel_all_inflight` 只 resume `slot.waiter` 非空的等待者；TCP fallback 期间 `slot.waiter` 为空（coroutine 挂在 `query_tcp` 内的 `stream.write/read` 上），**不会被唤醒**。若此时 `~DnsClient`/`release()` 释放 `slots_`，`query_tcp` 恢复后访问悬空引用 -> UAF/崩溃。`close()` 名义上"取消所有 inflight"却漏掉了 TCP fallback，对调用方有误导。

**修复**：把 in-flight `TcpStream`（或取消令牌）记入 slot，在 `cancel_all_inflight` 里 close 它，使 `query_tcp` 立即以错误返回；或在 `query_tcp` 每个挂起点后检查 `closing_` 并要求调用方 await 静默。

### 3. DNS 欺骗面：可预测的顺序事务 ID + 无 0x20 + 客户端不校验响应 question
`DnsClient.cpp:458-466`(`next_id_++` 从 0 线性递增) / `529-557`(handle_udp_packet 仅按 ID+源地址匹配) / `encode_query` 无 0x20

16 位顺序 ID 易于预测（RFC 5452）。`dnssec_ok` 默认 false、无 0x20 大小写随机化。这恰好是 HIGH #1 的入口--能预测 ID 就能伪造"question 对、answer owner 错"的响应。客户端 `handle_udp_packet` 在解析层校验前就把 ID 匹配的包 memcpy 进 slot。

**修复**：用 CSPRNG/随机起点初始化 `next_id_`；加 0x20；在 `handle_udp_packet` 里比对 question 段（解析层 `DnsResolverLocal.cpp:628` 已做，但客户端先拷贝了）。

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

1. **`retry_from_cache` 是死字段**：`DnsResolverLocal.h:108` 声明，`.cpp:650/792/805` 写，从未读。删除或显式 `if(outcome.retry_from_cache) continue;`。
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

| 优先级 | 条目 | 工作量 |
|--------|------|--------|
| P0 | HIGH #1（循环上限 + complete/release 顺序 + answer owner 校验） | 小 |
| P0 | HIGH #2（TCP fallback 取消） | 中 |
| P0 | HIGH #3（随机 ID + 0x20 + 客户端 question 校验） | 小 |
| P1 | MEDIUM #4/#5/#6（缓存 LRU/清理/tombstone/NxDomain 一致性） | 中 |
| P1 | MEDIUM #7（堆分配 churn） | 中 |
| P2 | MEDIUM #8 + 其余 LOW | 排期 |

## 已验证为正确的部分

- `decode_name` 指针压缩：强制 `target < pos`（向后指）+ `jumps > packet_len` 循环断路（`DnsName.cpp:124,132`）。
- `encode_name` 标签长度 1..63 校验、空标签拒绝、根处理（`DnsName.cpp:35-58`）。
- `MessageParser::parse` 记录总数 uint32 累加无溢出、与 max_records 比较（`DnsMessage.cpp:157-162`）。
- `encode_query` EDNS OPT 记录格式正确（root 名、type=OPT、class=payload size、TTL=version+DO、RDLENGTH=0）（`DnsMessage.cpp:244-253`）。
- `DnsCache` open-addressing tombstone 语义：find 跨过 tombstone、insert 复用首个 tombstone、erase 置 tombstone 保可达性（`DnsCache.cpp:255-301,377-391`）。
- `ensure_capacity` 字节记账：`bytes_used_ - old_blob_size + new_blob_size`，old 来自 entry 当前 blob_size，无下溢（`DnsCache.cpp:441,486,514`）。
- blob 对齐：地址记录区 `align_up(., alignof(IpAddress))`，`new char[]` 默认对齐 >= alignof(IpAddress)（`DnsCache.cpp:472,491`）。
- `handle_response` 校验响应 question 段匹配 qname/qtype/qclass（`DnsResolverLocal.cpp:617-631`）。
- SOA 负 TTL = `min(record.ttl, minimum)`（RFC 2308）（`DnsResolverLocal.cpp:106`）。
- literal IP 直返不查 DNS、按 policy 过滤（`DnsResolver.cpp:266-281`）。
- `RWFd::close()` 同步 resume 读等待者，`recv_loop` 在析构前被排空，无 UAF（`RWFd.cpp:38-71`）。
