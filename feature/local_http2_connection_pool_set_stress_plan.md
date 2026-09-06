# LocalHttp2ConnectionPoolSet 健壮性压测方案

状态：设计基线。已实现专用客户端及场景控制脚本，实际接口、执行档位和覆盖限制见 [运行说明](../scripts/benchmark/http2_pool/README.md)。本文中的完整矩阵仍是目标，不代表每个组合或长时档位均已运行。

## 1. 目标与测试边界

验证真实 Nginx HTTP/2 连接上，在多 loop、高并发、频繁建连/退休、请求取消和后台管理操作交错时，连接池是否保持容量、归属、生命周期和排空不变量。吞吐与延迟用于发现退化，首要通过条件是无数据串流、无悬挂、无越限、无资源持续增长，故障解除后能够恢复。

主链路：

```text
场景控制器 ──负载/取消/clear/shutdown──> 专用 C++ 压测客户端
                                           EventLoopGroup(W)
                                           LocalHttp2ConnectionPoolSet
                                           每个 loop 一个 core
                                                   │
                                           Http2PooledExchange
                                                   │ h2c / TLS h2
                                                   ▼
                                            Nginx 静态后端
```

客户端直接链接 `fiber_lib`，每个请求必须通过目标 set 取得 Lease。当前 `apps/lite_nginx/src/upstream/ConnectionPool.cpp:139` 使用 HTTP/1 池，直接向该网关施压不能覆盖本功能。外部 HTTP 压测工具只能用于校准 Nginx 能力，不能替代本客户端。

先在同机回环接口验证正确性；性能阶段将客户端和 Nginx 绑定不重叠 CPU，记录核数、亲和性、CPU 频率策略、内核、编译器、allocator、git revision、配置摘要和随机种子。有条件再在双机环境复验网络抖动。后端容量不足或发压线程饱和的轮次不能用于判定连接池性能上限。

## 2. 按当前实现定义验收口径

| 实现约束 | 压测必须遵守/检查的行为 |
|---|---|
| 每个 loop 独立 core | 所有 acquire、try_acquire、Lease/exchange 的使用和销毁、指标读取都在所属 loop；不得跨 loop 归还 Lease |
| 上限按 shard 生效 | 每 shard `connection_total <= max_connections_total`；每 shard/key 的 total 不超过 `max_connections_per_group`；进程上限为 `W × max_connections_total` |
| total 包含拨号和 draining | GOAWAY 后旧连接尚未释放时，新拨号也不能突破上限；不能仅数 ESTABLISHED 或 ready 来判断越限 |
| SETTINGS 与 Lease 是两层准入 | SETTINGS 生效后的稳定容量为 `min(对端上限, 非零本地软上限)`；预 SETTINGS 和上限缩小后的已发 Lease 单独记录，实际 attach 受 stream gate 控制 |
| 同 key 复用忽略新 connector | host、port、scheme、affinity 不同的身份分别建 key；测试同 key 更换 connector 时仍复用原连接 |
| 失败拨号在 acquire 内退避重试 | dial failure 不等同请求失败；最终 acquire 通常是 TimedOut/Canceled，分别统计每次拨号错误和最终结果 |
| try_acquire 只复用 | miss 不拨号、不创建 group、不抢已有等待者；`acquire(..., 0ms)` 无容量时为 Busy |
| clear 可复用、shutdown 永久拒绝 | clear/shutdown 使旧连接退休，但持有的 Lease 必须由业务完成/取消并释放；管理 API 不负责强行销毁业务对象 |
| clear_async 等待整个 core 的 join | 不断发起新请求可能延长等待；正常清理先关闭注入入口，竞态场景只允许有界数量的晚到请求 |
| 多个 shutdown 调用者共享排空 | 由不持有 Lease 的管理协程发起，全部调用者最终完成；set 和 EventLoopGroup 生存期覆盖全部请求及管理协程 |

依据：`include/fiber/http/LocalHttp2ConnectionPoolSet.h:33`、`src/http/LocalHttp2ConnectionPoolSet.cpp:59`、`src/http/LocalHttp2ConnectionPoolSet.cpp:161`，以及 [连接池使用文档](../docs/http2-connection-pool.md)。

## 3. Nginx 后端与数据集

采用 [nginx_backend.conf](../scripts/benchmark/http2_pool/configs/nginx_backend.conf)，普通端口 18082 宣告 128 streams，受限端口 18083 宣告 1 stream。两端口默认都使用 h2c prior knowledge，不走 HTTP/1 Upgrade。通过响应 `X-Backend-Protocol: HTTP/2.0`、客户端协议状态和短时抓包确认真正使用 HTTP/2。

仓库构建脚本启用了 HTTP/2、SSL，禁用了 rewrite；配置使用静态文件和 `try_files`，不依赖 `return`、Lua、echo 或 stub_status 模块。配置路径以仓库根目录作为 `-p`，PID、临时文件和日志单独存放，不复用 `scripts/nginx.conf` 的实例。

从仓库根目录执行以下准备命令；若 pinned 源码的 configure 或安装二进制缺失，先执行 `cmake -S . -B build`、`scripts/build_nginx.sh`：

```bash
python3 - <<'PY'
from pathlib import Path
import hashlib
import json

root = Path('build/http2-pool-bench/www')
root.mkdir(parents=True, exist_ok=True)
manifest = {}
for name, size in [('small.bin', 1024), ('medium.bin', 65536), ('large.bin', 1048576)]:
    body = (bytes(range(256)) * ((size + 255) // 256))[:size]
    (root / name).write_bytes(body)
    manifest[name] = {'bytes': size, 'sha256': hashlib.sha256(body).hexdigest()}
(root.parent / 'fixtures.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
temp/nginx-install/sbin/nginx -t -p "$PWD/" -c scripts/benchmark/http2_pool/configs/nginx_backend.conf
temp/nginx-install/sbin/nginx -p "$PWD/" -c scripts/benchmark/http2_pool/configs/nginx_backend.conf
# 只控制此专用实例；reload 前先 -t 检查本轮生成的配置。
temp/nginx-install/sbin/nginx -p "$PWD/" -c scripts/benchmark/http2_pool/configs/nginx_backend.conf -s reload
temp/nginx-install/sbin/nginx -p "$PWD/" -c scripts/benchmark/http2_pool/configs/nginx_backend.conf -s quit
```

`/small.bin` 为复用和吞吐基线；`/medium.bin`、`/large.bin` 为体完整性和流控；`/slow.bin` 返回 large.bin，按请求限速 64KiB/s，约需 16s。限速存在初始突发，不能用固定 sleep 推断流已经挂起，须以客户端已读取响应头/部分 body 的屏障触发故障。所有请求带唯一 `X-Bench-Id`，必须检查回显 ID、后端端口、状态码、完整长度及内容；相同静态 body 的校验不能单独证明请求未串流。

正确性轮次逐字节检查固定内容；吞吐轮次仍检查全部响应长度/ID，并明确报告内容校验采样比例。gzip 关闭，预热文件页缓存。日志包括协议、请求 ID、worker PID、连接号及该连接请求数；跨 reload/restart 使用“后端运行代号 + PID + connection”标识，不能把 `$connection_requests` 当 stream ID。

TLS h2 作为独立配置变体：增加专用 SSL 端口、测试证书、`http2 on`，connector 设置对应 SNI/信任链并要求 ALPN 为 h2；key 使用 Https。证书路径在生成配置时展开为绝对路径，先 `nginx -t`。包括完整握手、复用连接和故意失败的信任校验；禁止悄悄回落 HTTP/1。基线配置本身不包含 TLS。

已核对的 Nginx 行为：

- `http2_max_concurrent_streams` 为每连接并发上限；源码在 `temp/nginx-1.31.3/src/http/v2/ngx_http_v2.c:1284` 检查并发，在 `:2772` 构造 SETTINGS。语法见 [Nginx HTTP/2 模块文档](https://nginx.org/en/docs/http/ngx_http_v2_module.html#http2_max_concurrent_streams)。
- `keepalive_requests` / `keepalive_time` 可用于连接退休；该版本在处理 HEADERS 时触发 GOAWAY，见同文件 `:1348`，不是独立的精确到期计时器。配置语义见 [Nginx core 文档](https://nginx.org/en/docs/http/ngx_http_core_module.html#keepalive_requests)。
- graceful close 处理在同文件 `:360`；reload 测试仍须观测实际 GOAWAY/关闭及 worker 换代，不能仅以信号发送成功认定覆盖。
- reload 修改 stream 上限不会给同一条既有连接确定性注入“先缩小再增大”的 SETTINGS；该场景沿用现有协议测试或另加帧级测试器。Nginx 压测仅验新连接采用新上限，不声称覆盖动态 SETTINGS、畸形帧和指定 last-stream-id。

## 4. 专用压测客户端设计

已增加 `example/http2_pool_benchmark.cpp` 和 `scripts/benchmark/http2_pool/run.py`，提供 CLI、JSONL 及配置/命令存档。下述内容保留原始设计目标；已实现的参数和精度限制以运行说明为准。

每个 loop 预建 connector 上下文、BufPool、请求槽位和统计桶，初始化 set 后再启动工作。同步热路径使用 `try_acquire`，miss 后 `co_await acquire`；另设纯 acquire 对照。一个 Lease 对应一个 `Http2PooledExchange`，读到 END_STREAM 后销毁；取消时 reset，先终止流再归还名额。预留裸 Lease 有界持有模式，用于确定性占满容量与验证排空等待。

Connector 必须调用真实 `Http2ClientConnection::connect()`。拨号中取消场景可在回调内加入支持析构取消的 sleep 屏障，后续仍连 Nginx；分别标明“受控挂起”和真实网络故障结果。connector 参数存活至 acquire 结束，不能使用脱离父协程生命周期的后台拨号。

双负载模型：

1. 闭环：固定在途请求数，完成后补发，用于并发、复用与生命周期压力。
2. 开环：按计划时间发请求，预分配并限制全进程在途数及排队数，超限计 `loadgen_rejected`，不得无限创建协程。记录计划时间到完成的延迟、实际开始到完成的延迟和调度滞后，防止仅统计成功请求或闭环自降速掩盖过载。

普通请求默认 TCP timeout 1s、acquire 2s、整请求预算 5s；每次 header/body 操作取剩余总预算，不能每块 body 重新获得完整超时。slow 场景使用 30s 总预算，主动取消场景为 5–200ms。少量无限 acquire 只用于专门的 shutdown/取消验证，并由独立进程 watchdog 限时。时间源使用 `EventLoop::current().now()`。

## 5. 负载矩阵与运行档位

不做所有参数的笛卡尔积。先测默认值，再单因素边界，最后组合高风险参数；每轮保存完整 options，而不是只保存覆盖项。

| 维度 | 取值 |
|---|---|
| loop 数 W | 1、2、4、8（按机器实际核心限制）；均衡和 90% 请求落一个 loop |
| 每 shard 活跃 key 数 | 1、4、64、1024；有限 key 集合循环淘汰，另设新增 key 扩容阶段 |
| 每 shard/group 连接上限 | 1、2、4；每 shard 总连接上限 1、8、64，关联调整 idle 上限合法性 |
| stream 软上限 | 1、8、32、128、0；Nginx 上限 1、8、32、128 |
| 首 SETTINGS 前名额 | 0、1、16；冷启动反复建立真实连接 |
| 每组同时拨号 | 1、2、4；设置不超过该轮连接预算 |
| idle / lifetime | idle 预算 0、1、16；idle timeout 100ms、1s、60s；lifetime 0、1、17、1000 |
| 并发 | 每 loop 1、8、32、128、512；另测有效容量 B 的 B−1、B、B+1、2B、8B |
| 请求组合 | 100% small；90% small + 9% medium + 1% slow；100% large；取消比例 1%、10%、50% |

单 key 的稳定 Lease 容量估算 `B = max_connections_per_group × min(peer_limit, soft_limit)`，soft_limit 为 0 时只取 peer_limit，并受 shard 总连接预算约束。B 边界场景用持有屏障保证真正饱和，不依赖快速响应恰好重叠。多 key 超出总连接预算时，不承诺每个 key 都独占连接或跨 key 严格公平。

| 档位 | 执行规模 |
|---|---|
| smoke | 单/双 loop，h2c，各 P0 场景 10–30s；管理操作至少 20 轮 |
| standard | 每性能点预热 30s + 测量 180s，重复 3 次；每种故障至少 20 次；管理竞态 1000 轮 |
| soak | 8h 固定有限 key 集合的混合负载；另跑 2h 建连/淘汰 churn；每 60s 一个有记录的故障或 clear 周期 |
| sanitizer | Debug + ASan/UBSan 跑 P0/取消/管理/多 key 各至少 10min；TSan 单独构建验证多 loop 管理路径，不能和 ASan 同启 |

先测无错误可持续吞吐 R（在途、排队和延迟不持续增长），开环按 0.5R、0.8R、1.0R、1.2R、1.5R 阶梯加载，再降回 0.5R。过载允许可解释的 Busy/TimedOut/发压拒绝，必须能恢复。sanitizer 成绩不与 Release 吞吐直接比较；自定义协程切换或第三方库报告需定位归因，不能批量屏蔽后宣称通过。

## 6. 场景及逐项通过条件

| ID / 优先级 | 场景与注入方法 | 必须观察到的结果 |
|---|---|---|
| S01 / P0 | 同 loop/key 冷启动 512 个 acquire；先单连接单飞，后允许多拨号 | 同时拨号不越限，复用真实连接；稳态一连接服务多个 stream，全部 ID/body 正确 |
| S02 / P0 | W=4 同 key 并发，继而 90% 热点落 loop0 | Lease/connector/回调均在对应 loop；各 shard 独立限额，其他 shard 空闲不会被热 shard 偷用 |
| S03 / P0 | 单连接单 stream，持有 Lease，按序排队 100 个请求；同步轮询与短 deadline 混入 | 同 key 未取消者按入队序获名额，轮询不能插队；取消者不再恢复、不占名额，后续请求继续完成 |
| S04 / P0 | key 数超过 shard 连接预算；不同 host/port/scheme/affinity；持续插入/回收 key | 无跨身份复用；归还/退休连接后其他组能前进；扩容不破坏持有 Lease，停流清理后 group 为 0 |
| S05 / P0 | Nginx 1/8/128 streams，pre-settings 0/1/16；冷连接与稳态分开 | SETTINGS 后遵守对端准入；pre=0 等待首 SETTINGS，不额外拨号风暴；无非预期拒绝流/协议错误 |
| S06 / P0 | 只读 header 后 reset、body 中途 reset、持有未开流 Lease 后释放，取消 1%→50% | 流取消先于槽位归还；其他 stream 持续完成；停止取消后容量恢复，无在途计数残留 |
| S07 / P0 | 100% large 与慢消费者混合，暂停读取 100–500ms，控制暂停流数量 | 完整 body 不丢字节；触发流控后恢复读取仍推进；同连接竞争与全连接阻塞分别记录，不苛求慢流对短流完全零影响 |
| S08 / P0 | idle=0/1、idle_timeout=100ms/1s；峰值→停流→再峰值 100 轮 | idle 预算正确；超过 idle 时间且无 Lease 后最终回收；长持有流不被当 idle 淘汰，再次请求正常建连 |
| S09 / P0 | lifetime=1/17；Nginx keepalive_requests=17/100，混入 slow；受控 reload 20 次 | GOAWAY/退休连接停止新准入；允许此前已发 Lease 在 gate 层失败；合法在途流排空，替代连接服从 total 上限 |
| S10 / P0 | 专用 Nginx worker SIGKILL、实例停止后拒绝连接、停 1/5/30s 后恢复 | 每个请求在预算内完成/报错；失败拨号按组退避，回调可见；恢复后新请求成功，无持续损坏状态 |
| S11 / P0 | 专用 worker SIGSTOP 5s 后 SIGCONT，或隔离网络命名空间内针对测试端口丢包 | 已连请求与新建连接分别验证超时；取消无悬挂，恢复后连接池可用；故障控制器最终确保恢复被暂停进程 |
| S12 / P0 | 同时存在 active Lease、排队 acquire、挂起 connector，跨 loop clear_async | 执行第 7 节有界时序；旧等待/拨号取消，旧 Lease 释放后完成排空，新一轮重新成功 |
| S13 / P0 | 不同 loop 同时 2/8 个 shutdown_async；另加 clear 与 shutdown 交错 | 所有调用者等到全 shard 排空；shutdown 生效后新 acquire 为 Canceled，try_acquire miss；无提前析构 |
| S14 / P1 | 1024 个 key 持续轮换、短 lifetime、idle=0、取消与故障组合 2h | bucket/entry 反复复用不产生 UAF；固定峰值资源的轮次呈平台，无 FD/内存随轮次线性增长 |
| S15 / P1 | TLS h2 稳态/建连 churn、错误信任链后恢复、不同 TLS profile 使用不同 affinity | ALPN 确认为 h2；错误握手纳入 dial error 和退避；不跨安全身份复用，恢复正确配置后可用 |
| S16 / P1 | acquire-only 对比 try_acquire+fallback，开环超载后降载，8h soak | 健康段无非预期错误；队列有界、无永久饥饿；吞吐、p99、资源趋势满足第 9 节门槛 |

S04 的 host/affinity 可映射到同一个真实 Nginx 地址，隔离证据来自 connector 的连接代号与 key 绑定；端口隔离额外核对响应头。读取 `Lease::key()` 后应立即复制所需标量或摘要，不能把引用跨 await 保存。

S09 不开启业务自动重试来掩盖首尝试错误。GOAWAY 边界上未被后端接纳的请求可以失败，单独记录；健康且低于容量时的无故失败不能归入故障白名单。如补充业务重试轮次，仅对 GET 有界重试，并分别报告逻辑请求数、物理尝试数及首尝试错误。

Nginx 静态后端不会验证上传 body 的完整消费、gRPC/trailer 或指定 RST_STREAM 帧；这些不列为本轮已覆盖项。已有 `tests/Http2ConnectionPoolTest.cpp` 的 SETTINGS shrink/grow、GOAWAY、FIFO、取消等精确测试必须继续保留，用它们补充真实后端无法确定性构造的协议边界。

## 7. clear / shutdown 的可复现时序

为管理操作设置独立 epoch 和各 loop 的屏障，管理协程自身不持有 Lease。

1. 用不同 key 构造三类状态：A 持有真实 Nginx Lease/slow stream，B 被 A 同组容量阻塞，C 在 connector 的可取消挂起点。保存实际进入状态的计数，达到屏障才注入。
2. 通知全部负载 loop 停止接收新任务并确认；保留已在途任务。竞态轮次可额外放行固定数量的晚到请求，随后也关闭入口。
3. 在一个或多个 loop 发起管理任务。A 仍持有时验证管理尚未完成；观察 B/C 取消（竞态中晚到请求允许成功或取消，按其实际状态分类）。
4. 由独立请求拥有者在所属 loop 完成/reset A，并释放其他所有 Lease/exchange。释放动作不能等待管理完成，否则是测试自身死锁。
5. 等待所有管理调用者返回，向每个 loop 发采样任务，确认 connection/idle/group/在途请求/拨号全部为 0。
6. clear 轮次重新打开入口，验证 1000 个成功请求后进入下一轮；shutdown 轮次验证拒绝新 acquire，再等待请求及控制协程退出，stop/join group，销毁 set。下一轮构造新的 group/set。

普通清理门槛为“最后一个业务 Lease 释放后 5s 内完成”；另记从管理发起到返回的总时长，区分业务持有和排空延迟。外部 watchdog 30s 无进展时采集线程栈、最近事件和 shard 快照后终止本轮；它必须独立于被测 EventLoop，以捕获 loop 完全卡死。

## 8. 观测与一致性核对

每秒输出按 loop/key 聚合指标；只在所属 loop 注册/调用池回调与读取状态，通过每 loop 独立缓冲区交给控制器汇总。回调只记计数和有界事件，不能重入 acquire/clear 或阻塞写盘。

| 指标 | 来源/口径 |
|---|---|
| 请求守恒 | scheduled = loadgen_rejected + started；started = success + terminal_error + canceled + inflight；每个请求只能终结一次 |
| 资源守恒 | acquired_leases − released_leases = held_leases；connector_started = completed_ok + completed_error + canceled + active；排空后 held/active/inflight=0 |
| 请求/排队延迟 | acquire、send_header（含 stream gate）、header、body、总延迟的 p50/p95/p99/p99.9/max；超时/取消另分布；开环包括调度滞后 |
| 池状态 | 当前 loop 的 connection_total/idle_total/group_count；count callback 的 key/total/ready，维护各 key 最新值，不能把回调累计相加 |
| 拨号 | connector 开始/完成/取消、每 key 同时拨号峰值、dial_failed 的 IoErr/连续失败次数/retry_after；校验 10ms→20ms→…→1000ms 默认退避及成功复位 |
| 实际 stream 与连接 | connector 分配单调 connection generation；以 loop、generation、stream_id 关联请求；持有效 Lease 时读取 `http2().local_active_stream_count()` 与 peer SETTINGS/GOAWAY 状态 |
| 系统资源 | 客户端/后端分别记录 CPU、RSS/PSS、线程、FD、socket 状态、上下文切换、重传；记录 allocator live/retained（可用时） |
| 管理/故障 | epoch、注入/恢复时刻、每 shard 首次取消/最后释放/排空时刻、每个调用者完成、恢复至正常服务所需时间 |

池当前不直接公开 waiter 数、每连接 Lease 数和内部 free list 长度；客户端“acquire pending”包含排队和拨号，必须使用这个名称，不冒充内部精确队列长度。active lease 由客户端守恒计算，ready 表示可分配连接数，不能当可用 stream 数。需要内部状态时另加编译期开关下的只读观测，不为压测向热路径引入全局锁或逐请求字符串分配。

事件环使用预分配定长记录；保留最近 N 条状态变更，失败时落盘。连接地址会复用，不以裸指针作为跨生命周期唯一 ID。计时、分桶、内容校验的开销单列；性能轮次可关 access log，但正确性轮次必须保留。跨 loop 快照有时间偏差，硬越限断言应在本地状态变化时检查，最终归零则在停流屏障后检查。

## 9. 通过门槛与失败归因

以下数值为首轮建议门槛，执行前写入结果 manifest；不能看到失败后再放宽并覆盖旧结果。

- **硬门槛**：零崩溃、零断言、零 sanitizer 未解释报告、零重复完成、零串 ID/体损坏、零分片/分组连接越限。健康且低于可持续容量的测量段，非预期错误为 0。
- **进度**：每个有限 deadline 请求都有终态；功能轮次超时容差 `max(100ms, 5 × 已测 loop 调度滞后 p99)`，硬件过载导致超容差也需报告。管理满足第 7 节门槛，不接受只强杀后退出。
- **故障分类**：按注入窗口和受影响请求关联允许 transport error、TimedOut、Canceled、GOAWAY 拒绝；协议错误必须定位。注入窗口外持续失败，或非目标 key 失败，均单独调查。
- **恢复**：后端独立健康探针恢复且发压降至 0.5R 后，默认 10s 内进入连续 10s 的健康窗口（成功率 ≥99.9%，无新协议错误）；窗口之后稳定健康段要求零非预期错误。另测恢复吞吐至少为故障前同负载的 90%，p99 不超过 1.5 倍，排除预期限速请求。
- **排空**：清理后每 shard connection/idle/group 与客户端 Lease/拨号/请求计数为 0，客户端 FD 回到初始化后的基线；TIME_WAIT 单列，不按存活客户端 FD 泄漏处理。
- **内存**：固定 key/并发峰值并完成 10 轮预热后，至少比较后续 100 轮排空快照；末 10 轮 PSS 中位数相对初 10 轮增量不超过 `max(16MiB, 5%)`，且无持续正斜率。free list/allocator 保留高水位允许存在，不能要求 RSS 回进程启动值；疑似增长以 live allocation/泄漏诊断确认。8h soak 每小时检查同口径趋势。
- **性能回归**：相同机器/配置、重复 3 次中位数与保存的有效基线比较，吞吐下降超过 10% 或 p99 上升超过 20% 触发调查；没有历史基线时只建立基线，不把估算 RPS 写成验收成绩。

## 10. 实施顺序与交付

1. 实现最小客户端：真实连接、完整读体、请求守恒、每 loop 指标、全局有界停止；跑 S01/S02/S05。后端先做 h2c，继而 TLS。
2. 增加确定性屏障、FIFO、取消、idle/lifetime、clear/shutdown 和独立 watchdog，跑全部 P0。
3. 增加隔离故障控制、速率阶梯、多 key churn、sanitizer 与 soak。网络故障只能作用于本测试实例/命名空间；故障控制脚本用清理钩子恢复暂停进程及规则。
4. 实现后运行 `./format_code.sh`、`cmake -S . -B build`、`cmake --build build`、`ctest --test-dir build`；随后记录实际 benchmark CLI 和完整执行清单。新增确定性回归用例纳入 `fiber_tests`；长时 soak 不放入默认 CTest。
5. 每轮输出配置、版本、fixture hash、seed、summary.json、每秒指标、故障时间线、Nginx 日志、失败请求样本/最近事件，失败时附线程栈或抓包。报告按 S01–S16 标记 PASS/FAIL/未覆盖，并列出错误分类、资源趋势、恢复时间和剩余风险。

设计阶段交付包括本文及可校验的 Nginx h2c 配置；后续已补充专用客户端、TLS/故障配置生成器及运行脚本。实际运行结果单独记录，不以设计目标代替测量值。

本次已执行 fixture 生成命令及 `temp/nginx-install/sbin/nginx -t -p "$PWD/" -c scripts/benchmark/http2_pool/configs/nginx_backend.conf`，配置语法及文件检查通过；已运行 `./format_code.sh`（无 C++ 文件变更）和 `git diff --check`。未启动后端或执行压测，未改动 C++ 实现，因此本次没有运行 C++ 构建和 CTest。
