# TLS 自研实现 · 13 写路径：每次 try_write 批量封装、一次 flush

状态：已实现（2026-10-01），bench 见 §8。范围仅 TCP 面已连接阶段的写路径（`TlsStreamFd::try_write`），
外加 `TlsRecordCipher::seal()` 的 TLS 1.3 分支。`TlsConnection` 的 API、H1/H2 调用方都不改。

## 1. 动机（实测）

lite_nginx 代理 H2 GET 1 MiB over TLS 1.3（HEAD 23a73445，temp/bench，`h2_profile.sh` + `perf_h2.sh`），每个响应：

| 部分 | 耗时 | 说明 |
|---|---|---|
| 总 CPU | 1,084 µs | 4 个 worker 全部跑满 |
| sys：`sendmsg` | 65 次 / 378 µs | 每条 16 KiB record 一次 |
| sys：`recv` | 263 µs | 从上游读 1 MiB |
| user：memcpy | 35% | `try_write` 的 coalesce 31.7%，1.3 `seal()` 的 staging 拷贝 3.1% |
| user：AES-GCM | 24% | |

**根因**：`try_write` 每次调用只封装**一组**（一条合并的 record，或首节点里的若干整 record），
然后立即 `flush_output`。H2 的出站 chain 是 `[9 B 帧头][≤16 KiB payload][9 B 帧头]…` 交替排列，
首个 iovec 永远小于一个 record，于是每次调用只合并出一条 record，发一次 `sendmsg`。
H2 的 `pump_outbound` 虽然会循环调用 `try_writev`，但每次只推进 16 KiB。

**原型评估**（都在 `temp/ab` 的临时树里构建，没有进仓库）：

| 原型 | 做法 | 结果 |
|---|---|---|
| expS | 1.3 `seal()` 改走 `seal_scatter`，type 字节用 `extra_in` 传入，去掉 staging 拷贝 | H1s GET 1m 上 AES+memcpy 从 61.7% 变为 61.1%：拷贝读的是冷数据，去掉拷贝后 cache miss 成本转到 AES 上，**没有收益** |
| expSB | expS 加上每次 `try_write` 封装到 64 KiB 再 flush | `sendmsg` 从每响应 65 次降到 17 次；4 样本 A/B：**h2 get1m +7.0% RPS / −7.2% CPU per request，post1m +4.3% / −3.5%**（两组样本不重叠），get1k、get64k 持平 |

**排除的方案**：
- **gather seal**（用 `EVP_CIPHER` 流式 GCM 直接从多段输入加密，去掉 coalesce 拷贝）：coalesce 和 expS
  去掉的拷贝一样，都是第一次读冷数据，成本会转进 AES；省下的很少，却要多一套加密实现，ChaCha/CBC
  还得保留旧路径。
- **把 H2 帧头写进 payload 的 headroom**，让帧和 record 对齐：payload 是上游缓冲区的 slice，
  往 headroom 写会覆盖前一帧的数据，不可行。

## 2. 定谳

1. **`try_write` 每次调用连续封装多组 record**，直到累计明文 ≥ `kWriteBatchBytes` 或 chain 读完，
   然后只 flush 一次。批大小定为 64 KiB：实测 128/256 KiB 与 64 KiB 没有差别（§8），64 KiB 的
   已封装未写出内存最小。这是软上限：一批最多再多出一个 record。
2. **组的划分规则不变**，只是改为从游标位置继续：
   - 当前段是 chain 的最后一段：零拷贝，允许短 record；
   - 当前段 ≥ 一个 record：零拷贝封装其中的整 record，尾部留给下一组；
   - 否则：合并进 scratch，凑满 16 KiB。
   零拷贝组受批预算约束（按 record 取整，至少一个 record），所以 H1 的单个大节点（例如 1 MiB 的
   body）也不再一次封装整块：已封装未写出的密文最多一批，而不是整个节点。
3. **用游标（节点 + 偏移）遍历 chain**，不再每组都从头 `fill_write_iov`。coalesce 直接沿节点拷贝到
   16 KiB，不再受 16 个 iovec 的限制：由很多小节点组成的 chain 也能拼出满 record（现在最多拼 16 个
   节点，会产生很短的 record）。
4. **重试契约不变**：`pending_write_len_` 记整批明文长度；WouldBlock 时整批 record 留在
   `out_pending_`，同一条 chain 重试，其它 chain 报 Busy；整批 flush 完才 consume，并返回整批长度。
5. **失败语义**：批中任何一组 `conn_->write` 失败都整批报错，和现在单组失败时一致。
   - **Invalid**（连接已终态或已发 close_notify）只会出现在首组，此时什么都还没封装。照旧返回，
     不锁存：连接终态本身会让后续写继续返回 Invalid，`close()` 也照旧尽力发出读路径锁存的 fatal alert。
   - **NoMem**（连接致命）可能出现在任何一组，而且可能留下已封装但没报告给调用方的 record：
     多 record 的组中途失败，或批中后续组失败。序号已经推进，流的完整性无法再保证——继续写会让
     调用方重发的数据和这些 record 重复，再补一个 close_notify 会让对端以为是正常结束。
     因此 NoMem 时**锁存 `write_error_`**：之后的 `try_write`、`poll_shutdown` 直接返回该错误，
     `close()` 不再发 close_notify，也不再 flush。现在单组多 record 的 NoMem 也有同样的漏洞，
     这里一并封住。
6. **顺带**：1.3 `seal()` 在 dst 与明文不重叠时改走 `seal_scatter`，type 字节用 `extra_in` 传入。
   实测收益约为 0，但它是无风险的清理，此后 `seal()` 不再有任何拷贝。
7. **不做**：gather seal；每批一次性分配输出缓冲。`emit_sealed` 仍然每条 record 分配一次，
   malloc+free 约占 user 3%，留作后续。

## 3. try_write 新流程

```cpp
IoResult<size_t> TlsStreamFd::try_write(IoBufChain &buf) noexcept {
    // BadFd / write_error_ / 空 chain / Busy 检查（write_error_ 为新增）
    std::size_t batch_len = 0;
    if (out_pending_.empty()) {
        ChainCursor cursor(buf);   // 节点 + 偏移
        while (batch_len < kWriteBatchBytes && !cursor.at_end()) {
            const std::size_t room = kWriteBatchBytes - batch_len;
            const std::size_t record_room = std::max(kRecordPlaintextMax, room - room % kRecordPlaintextMax);
            const std::span<const std::uint8_t> seg = cursor.segment(); // 当前节点剩余的连续字节
            if (cursor.last_segment())            → 零拷贝 min(seg.size(), record_room)（可为短 record）
            else if (seg.size() >= kRecordPlaintextMax) → 零拷贝 min(整 record 部分, record_room)
            else                                  → 从游标沿节点拷贝至多 16 KiB 进 scratch
            if (!conn_->write(...)) { if (err == NoMem) write_error_ = err; 清 pending; return err; }
            cursor.advance(len);
            batch_len += len;
        }
        pending_write_chain_ = &buf;
        pending_write_len_ = batch_len;
    } else {
        batch_len = pending_write_len_;  // 重试：整批已封装
    }
    // flush_output / WouldBlock / consume_and_compact(batch_len) 与现在相同
}
```

`ChainCursor` 是 `TlsStreamFd.cpp` 里的局部辅助类型，基于 `IoBufChain::front_node()` 只读遍历，
跳过空节点。`last_segment()` 表示当前节点之后再没有可读节点。

## 4. 影响面

- **H2**：一次 `try_writev` 最多写约 64 KiB（4 个 record），`sendmsg` 次数约为原来的 1/4。
  `pump_outbound` 的循环和 hook 完成逻辑都按返回的字节数推进，不用改。
- **H1**：上游读来的节点一般不超过 64 KiB，行为和现在接近；单个大节点（应用直接写 1 MiB）由一次
  封装整块变成每批一次 flush，已封装未写出的内存被限制在一批以内。syscall 次数是否变化要实测
  （h1s get1m）。
- **时延**：第一个字节要等一批（约 64 KiB）封装完才发出，大约 6–10 µs；以前封装 16 KiB 就发。
- **内存**：`out_pending_` 里的密文最多一批多一个 record。

## 5. 测试

- `TlsStreamFdTest` 现有 4 个写用例里，3 个只断言数据完整和总字节数，原样通过。
  `TlsTransportWritevFillsRecordsAcrossSmallAndLargeNodes` 原来用"每次调用返回的字节数"代替
  record 大小，批量后两者不再一致；改为在客户端和服务端之间放一个中继线程，解析经过的 TLS record
  头，直接断言线上 record 的明文长度（TLS 1.3：16384 × 4 + 3952，之后是 2 字节的 close_notify），
  比原来更严格。
- 新增：
  - H2 形状的 chain（`[9][16384]` × 8）：每次调用返回的字节数不超过 `kWriteBatchBytes` 加一个
    record，对端收到的字节逐一一致；
  - 2000 个 10 字节的小节点：每次合并都能拼满 16 KiB（以前最多 16 个节点，即 160 B）；
  - 1 MiB 单节点：每次调用的字节数有上限。
  - NoMem 锁存没有可靠的注入手段（分配失败无法从测试里触发），靠代码审查保证。
- `TlsRecordCipherTest` 已有的 1.3 seal 向量（含 dst 与明文不重叠的情况）覆盖 `seal()` 的改动。
- fuzz：`TlsConnection` 的 API 没变，corpus 回放即可。

## 6. Bench 计划

1. 批大小：64 / 128 / 256 KiB，在 h2 get1m、h2 post1m、h1s get1m 上各 4 个交替样本。
2. 选定批大小后，HEAD 对新实现做 A/B：h2 和 h1s 各四个场景，4 个交替样本，记录 RPS 和 CPU/req。

## 7. 提交拆分

1. `perf(tls): seal TLS 1.3 records without staging the plaintext`：`seal()` 改走 `seal_scatter`。
2. `perf(net): seal a batch of records per TlsStreamFd write`：游标、批量封装、`write_error_` 锁存、测试。
3. `docs(tls)`：bench 结果补进本文。

## 8. Bench（2026-10-01，WSL2 i7-13700H）

方法与 feature/tls/12 §11 相同：temp/bench 的 lite_nginx 代理场景，h2 = h2load `-t6 -c32 -m16`，
h1s = wrk `-t6 -c256` 走 TLS，每轮 20 s，before/after 交替 4 个样本，CPU/req = 代理进程
utime+stime ÷ 成功请求数。所有二进制都在 `temp/ab` 的同一棵临时树里以相同配置构建。

**批大小**（before = 64 KiB）

| 协议 | 场景 | 128 KiB Δ RPS / Δ CPU | 256 KiB Δ RPS / Δ CPU |
|---|---|---|---|
| h2 | get1m | −1.2% / +0.9% | +2.1% / −0.3% |
| h2 | post1m | +0.7% / −0.8% | −1.8% / +1.4% |
| h1s | get1m | +0.1% / −0.6% | −0.9% / +0.0% |

全部在噪声内，样本范围重叠：H2 的出站编码水位本身是 64 KiB，chain 很少超过这个量。取 64 KiB。

**最终 A/B**：before = HEAD（21aea0dc），after = 本实现（64 KiB），64 轮，0 错误。

| 协议 | 场景 | RPS before | RPS after | Δ RPS | CPU µs/req before | after | Δ CPU/req |
|---|---|---:|---:|---:|---:|---:|---:|
| h2 | get1k | 189,247 | 194,703 | +2.9% | 21.1 | 20.5 | −3.0% |
| h2 | get64k | 38,868 | 42,024 | **+8.1%** | 102.5 | 95.4 | **−7.0%** |
| h2 | get1m | 3,534 | 3,830 | **+8.4%** | 1,125.8 | 1,041.3 | **−7.5%** |
| h2 | post1m | 1,697 | 1,786 | **+5.3%** | 2,349.5 | 2,223.9 | **−5.3%** |
| h1s | get1k | 129,583 | 132,192 | +2.0% | 30.8 | 30.2 | −2.0% |
| h1s | get64k | 55,227 | 56,347 | +2.0% | 72.2 | 70.5 | −2.4% |
| h1s | get1m | 5,603 | 5,738 | +2.4% | 707.9 | 689.6 | −2.6% |
| h1s | post1m | 2,869 | 2,893 | +0.8% | 1,400.4 | 1,389.4 | −0.8% |

**解读**
- h2 get64k、get1m、post1m 的 after 样本全部高于 before 样本（范围不重叠），是确定的提升：
  H2 的出站 chain 每条 record 都要合并，批量正好把每条 record 一次 `sendmsg` 变成每 64 KiB 一次。
- h2 get64k 在 §1 的原型 A/B 里是 +0.0%，这次是 +8.1%；那次原型的样本离散很大（36.4k–42.3k），
  这次两组都很集中。原型和本实现的差别（游标、合并不受 16 个 iovec 限制、单个大节点也受批预算约束）
  在这个场景里应当不起作用，所以更可能是那次的噪声；没有深究。
- h1s 三个 GET 场景都是 +2% 左右，方向一致但样本略有重叠；H1 的上游节点一般不超过 64 KiB，
  批量能合并的有限。
- h2 get1k 和 h1s post1m 在噪声内。

## 9. 后续：合并缓冲改为栈缓冲（2026-10-01）

`write_scratch_` 原来是每个连接懒分配的 16 KiB，连接只要合并过一次就一直占着，直到关闭。H2 连接
每帧都有单独的 9 字节帧头节点，H1 响应头节点后面跟 body 节点时也走合并，所以几乎每个写过数据的
TLS 连接都带着它。它比连接其余的 TLS 状态加起来还大：整个 `TlsTransport` 才 2,264 B。

它的内容只在 `seal_write_batch` 的一次调用内有效：每组先拷进去，`conn_->write()` 同步加密进
record 缓冲后，下一组立即复用。中间没有挂起点和回调，WouldBlock 重试保留的是已加密的 record，
连接跨 loop handover 也不跨调用持有它。因此改为 `seal_write_batch` 里的 16 KiB 栈数组
（不初始化，只读拷进去的前缀）：

- 每个连接少 16 KiB，约从 18.7 KB 降到 2.3 KB；
- 没有分配，也就没有 NoMem 分支；成员、懒分配和 `close()` 里的释放都删掉了；
- 同一线程上的所有连接共用栈上同一块内存，始终在 cache 里。

栈帧多 16 KiB：仓库里没有设置线程栈大小的地方，事件循环线程是 `std::jthread`，默认 8 MB。
只有把库嵌进栈很小的宿主线程时才需要改回 `thread_local` 的懒分配。

A/B（HEAD e204f07f 对比本改动，h2，4 个交替样本，0 错误）：get1k −0.2% RPS / +0.4% CPU，
get1m +1.5% / −1.1%，post1m +0.9% / −0.9%，都在噪声内。CPU 持平，收益在内存。
