# lite-nginx DNS 解析 + HTTPS 上游压测计划

## 1. 目标与边界

本计划验证 lite-nginx 使用域名发现 HTTPS 上游时的：

1. 功能正确性：A/AAAA、CNAME、TTL、NXDOMAIN、SERVFAIL、UDP 截断后 TCP
   回退、多地址连接回退、SNI、上游 TLS 和响应完整性。
2. 故障健壮性：DNS 延迟/丢包/错误响应、地址切换、旧上游下线、TLS 握手失败、
   上游中断、长时间抖动时不崩溃、不失控放大查询且能够恢复。
3. 性能：与仓库固定的 Nginx 1.31.3 比较成功 RPS、尾延迟、CPU 效率、内存、
   DNS 查询量和新建 TLS 连接成本。

主测试只使用下游 HTTP/1.1 明文监听，上游固定为 HTTP/1.1 over TLS 1.3。这样不会把
下游 TLS、HTTP/2 或 HTTP/3 的差异混入 DNS + HTTPS 上游结论。下游 HTTPS/H2 可在主
结论完成后作为扩展矩阵运行。

## 2. 已确认的实现语义

### lite-nginx

- DNS 服务器取自进程看到的 `/etc/resolv.conf` 第一条 `nameserver`，端口固定为 53；
  没有可配置的 `resolver` 指令
  （`apps/lite_nginx/src/runtime/DnsService.cpp:24-55`）。
- 每个 worker loop 有独立 resolver，共享一份 DNS cache；单次查询超时 2 秒、最多
  2 次尝试（`apps/lite_nginx/src/runtime/DnsService.cpp:62-100`）。
- 正 TTL 被限制在 1～300 秒，负 TTL 被限制在 1～60 秒
  （`include/fiber/dns/DnsResolverLocal.h:64-72`）。
- 命中空闲连接池时不做 DNS 和建连；只有连接池 miss 才解析域名
  （`apps/lite_nginx/src/upstream/UpstreamConnection.cpp:16-38`）。
- 解析结果按 IPv6 优先排序，连接失败时依次尝试后续地址
  （`apps/lite_nginx/src/upstream/UpstreamConnection.cpp:52-98`）。
- HTTPS 上游会把域名作为 SNI，但当前上游客户端默认不校验证书
  （`apps/lite_nginx/src/upstream/UpstreamConnection.cpp:43-49`、
  `include/fiber/net/TlsOptions.h:96-116`）。

这意味着“TTL 到期”不等于正在使用的池化连接会立即迁移。只要旧连接仍可复用，
lite-nginx 就不会再次查询 DNS；地址切换必须单独测量“旧连接仍健康”和“旧地址下线”
两种情况。

### Nginx 1.31.3

本计划使用 `scripts/build_nginx.sh` 固定的 Nginx 1.31.3。运行时解析使用
`upstream zone`、`server ... resolve` 和显式 `resolver`：

- `resolve` 要求 upstream 位于共享内存且必须配置 resolver
  （`temp/nginx-1.31.3/src/http/ngx_http_upstream_round_robin.c:93-134`）。
- Nginx 由定时器按解析结果的有效期在后台刷新 upstream peer
  （`temp/nginx-1.31.3/src/http/modules/ngx_http_upstream_zone_module.c:691-708`、
  `:1019-1033`）。

因此 Nginx 和 lite-nginx 的 DNS 刷新架构并不相同。报告必须把“请求数据面性能”和
“DNS 控制面行为”分开，不能把 TTL=1 的结果解释成完全同语义实现的直接胜负。

## 3. 测试拓扑

### 3.1 单机回归拓扑

```text
h2load -> SUT(18080) -> HTTPS backend A/B(19443) -> origin(19001)
                     \-> CoreDNS(53, metrics 9153)
```

- SUT：lite-nginx 或 Nginx，只能同时启动一个。
- HTTPS backend A：`127.0.0.1:19443`。
- HTTPS backend B：`127.0.0.2:19443`，用于 DNS 切换和多 A 记录。
- origin：现有 `build-bench/example/http_benchmark_backend`。
- DNS：CoreDNS 绑定 `127.0.0.53:53`，Prometheus 指标绑定
  `127.0.0.53:9153`。

建议在专用容器或 network namespace 内运行整套拓扑，并让 lite-nginx 看到：

```text
nameserver 127.0.0.53
options attempts:1 timeout:1
```

`options` 不会改变 lite-nginx 内建的 DNS 超时/重试参数；它只是避免同一 namespace
内其他工具使用外部 DNS。CoreDNS 绑定 53 端口需要容器 root 或
`CAP_NET_BIND_SERVICE`。不要直接改宿主机 `/etc/resolv.conf`。

单机结果适合回归和相对比较，不代表跨机生产容量。正式容量结果建议使用三台同规格
裸机（load generator、SUT、DNS + backend），固定 IRQ/NUMA/CPU 频率并记录网卡丢包。

### 3.2 CPU 隔离

沿用 `scripts/benchmark/http/run_matrix.sh` 的方法，但先根据实际机器拓扑重新选核：

- SUT：同一 NUMA node 的 2 个物理核，禁用 SMT sibling。
- load generator：4 个独立物理核。
- origin + HTTPS backend：2～4 个独立物理核，确保不是瓶颈。
- CoreDNS：1 个独立物理核。
- 所有服务使用同一套 cgroup CPU/内存统计；测试期间不得发生 CPU throttling。

先直接压 origin 和 HTTPS backend，确认其容量至少是预期 SUT 峰值的 1.5 倍。

## 4. 配置文件

配置位于 `scripts/benchmark/dns_https/configs/`：

- `lite_nginx_sut.conf`：lite-nginx，域名 HTTPS 上游，per-loop keepalive 256。
- `nginx_sut.conf`：Nginx 等价组，动态 DNS upstream + 每 worker keepalive 256。
- `nginx_backend_a.conf` / `nginx_backend_b.conf`：两个 TLS 终止后端。
- `Corefile` / `db.dns-bench.test`：可控 DNS 和测试记录。

公平性设置：

- 两端均为 2 worker、访问日志关闭、响应/请求代理缓冲关闭。
- 两端的上游连接池均为每 worker/loop、每 peer 最多 256 条空闲连接。
- 上游 TLS 只允许 TLS 1.3，证书校验均关闭。
- 后端禁用 TLS session ticket；Nginx SUT 还显式设置
  `proxy_ssl_session_reuse off`，形成“完整握手归一化”主结果。
- lite-nginx 使用 `steal off`，因为 Nginx 没有等价的跨 worker 空闲连接窃取。
  `steal auto/on` 只作为 lite-nginx 自身优化的补充组。

生产安全性必须另行验收：当前两份 SUT 配置都不校验上游证书，这只是为了匹配
lite-nginx 当前行为，不应直接复制到生产。

## 5. 测试矩阵

### 5.1 正确性和健壮性

每个用例同时记录客户端状态码/响应哈希、SUT 日志、CoreDNS 指标、连接数、SUT
是否存活、故障结束后的恢复时间。

| ID | DNS/上游操作 | 预期与记录重点 |
|---|---|---|
| F01 | `backend-long`，冷启动 | 首批并发请求最终 200；body hash 正确；后端响应头中的 SNI 为测试域名 |
| F02 | A 正常、AAAA NODATA | A/AAAA 均被正确处理，无错误和查询风暴 |
| F03 | 100/500/1500ms DNS 延迟 | 冷请求延迟随注入增长；后续缓存/池命中不受影响 |
| F04 | DNS 丢包 5s | 返回 502/504 而非挂死；lite-nginx 最坏 DNS 等待约 4s；恢复后成功 |
| F05 | NXDOMAIN，1s 后恢复 A | 验证负缓存；记录从切换到首个 200 的时间和期间 5xx |
| F06 | SERVFAIL，1s 后恢复 | 记录是否继续使用旧 peer、错误数和恢复时间；两端语义可不同 |
| F07 | UDP 响应置 TC，TCP 正常 | 两端完成 TCP fallback 并返回 200 |
| F08 | 1～8 跳 CNAME；第 9 跳 | 1～8 跳成功；超限时有界失败且服务仍健康 |
| F09 | `backend-fallback`：一个地址拒绝、一个地址正常 | 冷请求和轮转命中坏地址时回退到可用地址；记录额外连接延迟 |
| F10 | `backend-switch` 从 A 改到 B，A 保持健康 | 记录两端多久采用 B；lite-nginx 允许继续复用 A，不能误判为 TTL 失效 |
| F11 | A 切到 B 后停止 A | 记录 5xx 数、最长失败窗口和恢复时间，持续健康探测 |
| F12 | TLS 后端无证书/握手中断/只支持 TLS 1.2 | 有界 502，SUT 不崩溃，恢复后 200 |
| F13 | 上游 close/partial/hang | 复用 origin 现有 fault 路径，验证超时、连接回收和后续健康 |
| F14 | DNS A/B 每秒切换，持续 30 分钟 | 无 crash、无持续增长 RSS/FD/连接/DNS inflight |
| F15 | 正常流量中重启 DNS 100 次 | 已有连接不应被无关中断；需要新连接时有界失败并恢复 |

F04 的两端等待时间不是天然相同：lite-nginx 当前固定为 2 秒 × 2 次，Nginx 配置为
`resolver_timeout 4s`。必须在原始结果中保留实际时间线，不只比较最终状态码。

建议验收门槛：

- 正常阶段响应哈希 100% 正确，非故障窗口 5xx/timeout 为 0。
- 所有故障用例结束后 10 秒内恢复（地址切换用例另报精确恢复分布）。
- SUT 不退出、不死锁；30 分钟抖动后 RSS 增长小于 5%，FD 回落至稳态 ±5%。
- 冷启动和 TTL 刷新时 DNS 查询量必须是有界的；按 A/AAAA、worker、重试拆分，
  不用一个对两种架构都不合理的固定查询数作为门槛。

### 5.2 性能

请求集：

- GET 1 KiB：代理和 DNS/TLS 控制面的主指标。
- GET 64 KiB：响应转发效率。
- POST 64 KiB echo：双向流式转发。

场景：

| ID | SUT upstream | DNS TTL | 连接池 | 目的 |
|---|---|---:|---:|---|
| P00 | 直接 IP + HTTPS | 无 DNS | 256 | 纯 HTTPS 代理控制组 |
| P01 | `backend-long` + HTTPS | 300s | 256 | 生产稳态主对比 |
| P02 | `backend-short` + HTTPS | 1s | 256 | TTL 刷新控制面；观察池命中对 lite DNS 刷新的抑制 |
| P03 | `backend-short` + HTTPS | 1s | 0 | DNS cache + 每请求 TCP/TLS 新建的极限成本 |
| P04 | `backend-fallback` + HTTPS | 1s | 0 | 含坏地址的多地址集合及连接回退平均成本 |
| P05 | P03，但 Nginx 使用默认 TLS session reuse、后端开启 ticket | 1s | 0 | 生产能力差异补充组，不并入归一化主排名 |

配置变体只改以下内容，其他参数保持不变：

- P00：lite 的 upstream peer 改为 `https://127.0.0.1:19443`；Nginx 去掉
  `resolve`、使用 `127.0.0.1:19443` 并设置 `proxy_ssl_server_name off`。lite-nginx
  对 IP peer 不发送 SNI，这一控制组也应保持一致。
- P02：主配置中的 `backend-long` 改为 `backend-short`。
- P03：基于 P02，lite 设置 `keepalive_size 0`；Nginx 删除 upstream
  `keepalive 256`。
- P04：P03 的主机名改为 `backend-fallback`。
- P05：Nginx 删除 `proxy_ssl_session_reuse off`，backend 开启
  `ssl_session_tickets on`；lite-nginx 配置不变。

每个场景分两轮：

1. 容量轮：`h2load --h1 -D 60s --warm-up-time 15s`，并发从
   1/8/32/128/256/512 递增，找到无错误最大成功 RPS 和饱和拐点。
2. 延迟轮：使用 `h2load --rps`，分别固定为双方较低峰值的 50%、70%、85%，
   每档 120 秒；不要用各自峰值百分比，否则负载不是同一个绝对值。

每个数据点至少 7 次，lite/nginx 奇偶轮反转启动顺序，轮间冷却 30 秒。冷启动 DNS
延迟单独用 F01/P02 测量，不混入有 15 秒预热的稳态数据。

## 6. 指标与有效性

主指标：

- 成功 RPS，只计算完整 2xx 响应。
- p50/p95/p99/p99.9/max 延迟。
- SUT CPU seconds、requests/SUT CPU second、user/system CPU。
- RSS/MemoryPeak、FD、TCP established/TIME_WAIT。
- CoreDNS A/AAAA 查询数、响应码、请求时延；从 9153 指标端点采集前后 delta。
- 5xx、连接错误、超时、响应 hash 错误和恢复时间。

运行无效条件：

- origin/HTTPS backend 达到 CPU 上限或其 p99 在同组内漂移超过 10%。
- load generator 满 CPU。
- SUT cgroup 出现 throttling。
- 网卡/UDP/TCP drop 计数增长。
- 正常阶段有非预期状态码、响应 hash 错误或 SUT 异常退出。
- 测试期间发生系统更新、其他重负载或 CPU 频率策略变化。

报告同时给出每组 7 次中位数、min/max、lite/nginx 配对比值，以及配对比值的
bootstrap 95% CI。没有通过有效性门槛的数据保留原始文件，但不能进入性能结论。

## 7. 推荐执行顺序

1. 运行配置检查和 F01，确认 resolver、SNI、TLS 版本和响应 hash。
2. 完成 F02～F13；功能门槛未通过前不做性能排名。
3. 直压 origin/HTTPS backend，确认容量余量。
4. 运行 P00，建立纯 HTTPS 代理基线。
5. 运行 P01～P04；P05 只作为能力差异补充。
6. 运行 F14/F15 soak。
7. 保存二进制 hash、Git commit/status、Nginx `-V`、CMake cache、CPU/内核/
   sysctl/ulimit、完整配置、DNS zone、CoreDNS 版本与所有原始输出。

最终报告必须明确区分：匹配 Nginx 的行为、有意不同的行为、功能缺口，以及仍需扩大
样本或跨机验证的不确定结果。
