# HTTP/1 Connection Pool 使用文档

本文档说明当前仓库中的 HTTP/1 连接池分层、适用场景和使用方式，覆盖：

- 单 `EventLoop` 内核池：`Http1ConnectionPoolCore`
- 仅本地 loop 复用：`LocalHttp1ConnectionPoolSet`
- 支持跨 loop steal idle 连接：`StealableHttp1ConnectionPoolSet`

相关头文件：

- `src/http/Http1ConnectionGroupKey.h`
- `src/http/Http1ConnectionPoolCore.h`
- `src/http/LocalHttp1ConnectionPoolSet.h`
- `src/http/StealableHttp1ConnectionPoolSet.h`

## 1. 设计目标

当前连接池只缓存 HTTP/1 idle 连接，不做：

- 同组请求排队
- 建连去重
- active 连接数限流
- HTTP/2 风格多路复用

连接池的目标是：

- 复用同一个连接组内的 keep-alive 连接
- 控制 idle 连接数量和超时淘汰
- 在需要时支持跨 `EventLoop` 借用其它 loop 上的 idle 连接

## 2. 分层概览

### 2.1 `Http1ConnectionPoolCore`

`Http1ConnectionPoolCore` 是单个 `EventLoop` 上的连接池内核。

职责：

- 管理本 loop 的 idle 连接
- 按 `GroupKey` 分组
- 组内按 LIFO 复用
- 全局按最老 idle 连接淘汰
- 提供 `Lease`
- 支持把本 loop 的 idle 连接 detach 给 steal 层

### 2.2 `LocalHttp1ConnectionPoolSet`

`LocalHttp1ConnectionPoolSet` 是一个 facade。它为 `EventLoopGroup` 中每个 loop 各创建一个 `Http1ConnectionPoolCore`，然后在调用时根据 `EventLoop::current()` 自动路由到当前 loop 对应的 core。

适用场景：

- 你只希望在当前 loop 内复用连接
- 更看重响应路径简单
- 不在乎不同 loop 会建立更多连接

### 2.3 `StealableHttp1ConnectionPoolSet`

`StealableHttp1ConnectionPoolSet` 也是一个 facade。它同样为每个 loop 持有一个 `Http1ConnectionPoolCore`，但额外维护每个 loop 的 `Http1ConnectionGroupHintTable`，并在本地 miss 时按 hint 逐个向后探测其它 loop，尝试 steal 对应组的 idle 连接。

适用场景：

- 希望减少总连接数
- 同一组连接分散在多个 loop 上
- 可以接受一次跨 loop 探测和借用的开销

## 3. `Http1ConnectionGroupKey`

连接分组由 `Http1ConnectionGroupKey` 决定。

当前分组条件：

- host identity
- port
- scheme

其中 `scheme` 只有：

- `Http1ConnectionGroupKey::Scheme::Http`
- `Http1ConnectionGroupKey::Scheme::Https`

创建方式：

```cpp
using fiber::http::Http1ConnectionGroupKey;

auto name_key = Http1ConnectionGroupKey::from_name(
    "example.com",
    443,
    Http1ConnectionGroupKey::Scheme::Https);

auto ip_key = Http1ConnectionGroupKey::from_ip(
    fiber::net::IpAddress::loopback_v4(),
    8080,
    Http1ConnectionGroupKey::Scheme::Http);
```

注意：

- `from_name()` 会把域名归一化为小写
- 域名和 IP 是不同的 host kind，不会混用
- `http` 和 `https` 永远不会共用连接
- `example.com:443 + https` 与 `1.2.3.4:443 + https` 不是同组

建议：

- 如果上层请求目标是域名，就用域名建 key
- 如果上层是按 IP 直连，就用 IP 建 key
- 对 HTTPS，不要把不同域名压成同一个 IP key

## 4. `Lease` 语义

三种连接池接口最终都会返回一个 lease。

共同语义：

- `lease.valid()`
  - lease 是否有效
- `lease.hit()`
  - 是否命中已有 idle 连接
- `lease.has_connection()`
  - lease 当前是否已经持有一个 `Http1ClientConnection`
- `lease.get()` / `lease.connection()`
  - 访问连接对象
- `lease.reset()`
  - 归还连接或释放资源

重要语义：

- `hit() == true` 时，一定已经拿到了一个可用连接
- `hit() == false` 时，lease 仍然可能是有效的，只是当前没有连接对象
- 对 miss lease，可以调用 `emplace_connection(...)` 原地构造一个新连接
- `Lease` 析构时会自动 `reset()`

### 4.1 miss lease 的含义

本项目的连接池不是“miss 就返回空”。它返回的是一个有效 lease，只是里面还没有连接对象：

```cpp
auto lease = pool.acquire(key);
if (!lease.hit()) {
    auto conn_result = lease.emplace_connection(options);
    // 然后再 connect()
}
```

这样做的目的是：

- 避免在 acquire miss 时就提前分配 entry
- 让“本地 miss 然后新建连接”仍然走统一的 lease 生命周期

### 4.2 remote borrowed lease 的限制

只对 `StealableHttp1ConnectionPoolSet` 生效。

如果 lease 是从其它 loop steal 来的：

- `hit()` 恒为 `true`
- `has_connection()` 恒为 `true`
- 不允许再调用 `emplace_connection(...)`
- `lease.connection().loop()` 可能不是当前 `EventLoop::current()`

remote borrowed lease 用完后，会自动归还到它原本所属的 home loop/core，而不是回到当前 loop。

## 5. `Http1ConnectionPoolCore`

### 5.1 什么时候直接用 core

只有在你自己明确知道当前逻辑只会运行在单个 `EventLoop` 上时，才建议直接使用 `Http1ConnectionPoolCore`。

如果你已经有 `EventLoopGroup`，更常见的是用：

- `LocalHttp1ConnectionPoolSet`
- `StealableHttp1ConnectionPoolSet`

### 5.2 初始化

```cpp
fiber::http::Http1ConnectionPoolCore::Options options{};
options.max_idle_per_group = 2;
options.max_idle_total = 64;
options.idle_timeout = std::chrono::milliseconds(30000);
options.initial_group_capacity = 64;

fiber::http::Http1ConnectionPoolCore pool(loop, options);
if (!pool.init()) {
    // 内存不足或索引初始化失败
}
```

配置项说明：

- `max_idle_per_group`
  - 单个组最多保留多少条 idle 连接
- `max_idle_total`
  - 整个 pool 最多保留多少条 idle 连接
- `idle_timeout`
  - idle 超时时间；小于等于 0 表示所有 idle 连接都会立即过期
- `initial_group_capacity`
  - 分组索引的初始容量

### 5.3 获取和创建连接

```cpp
auto lease = pool.acquire(key);

if (!lease.hit()) {
    auto conn_result = lease.emplace_connection(client_options);
    if (!conn_result) {
        // 处理 NoMem / Invalid
    }

    auto &conn = **conn_result;
    auto connect_result = co_await conn.connect(std::chrono::seconds(5));
    if (!connect_result) {
        // 处理 connect 失败
    }
}

auto &conn = lease.connection();
```

注意：

- `Http1ClientConnection::connect(timeout)` 只能在连接所属 loop 上调用
- `timeout` 只限制 TCP connect 阶段；TLS 握手继续使用 `TlsOptions::handshake_timeout`
- `lease.emplace_connection()` 只负责在 entry 中构造连接对象，不会自动 `connect()`
- 如果 connect 失败，连接对象仍会随 lease 生命周期被清理

### 5.4 回池行为

`lease.reset()` 或析构时：

- 如果连接仍然 `reusable()`，会回到 pool
- 如果连接不可复用，entry 会被销毁并回收到 freelist

`reusable()` 的前提来自 `Http1ClientConnection`：

- 连接处于 idle 状态
- transport 仍然有效
- keepalive 仍可继续使用

### 5.5 淘汰策略

当前策略：

- 组内复用顺序：LIFO
- 组内超限淘汰：最老的 idle 连接
- 全局超限淘汰：全局最老的 idle 连接
- 过期清理：优先从全局最老 idle 开始

也就是：

- 最近刚归还的连接优先复用
- 最久没用的连接优先淘汰

### 5.6 线程/loop 约束

`Http1ConnectionPoolCore` 是 owner-loop 数据结构。下列操作要求在所属 loop 上执行：

- `acquire()`
- `try_steal_idle_entry()`
- `accept_returned_entry()`
- `sweep_expired()`
- `clear()`
- `shutdown()`

### 5.7 `clear()`

`clear()` 只清当前 pool 中的 idle 连接，不会主动取消已借出的 lease。

因此要保证：

- pool 对象生命周期长于所有未释放 lease

如果某条连接在 `clear()` 时已经被别的 loop 借走，那么它之后归还时仍然会重新回到 home core。

### 5.8 `shutdown()`

`shutdown()` 比 `clear()` 更强：

- 会先清当前 pool 中的 idle 连接
- 之后不再允许新的 `acquire()` / `emplace_connection()`
- 已经借出的 remote borrowed lease 在归还时不会再回池，而是直接销毁

## 6. `LocalHttp1ConnectionPoolSet`

### 6.1 适用场景

选择 `LocalHttp1ConnectionPoolSet` 的典型场景：

- 你运行在 `EventLoopGroup`
- 只希望在当前 loop 内复用 idle 连接
- 不希望本地 miss 后产生跨 loop 探测开销

### 6.2 初始化

```cpp
fiber::event::EventLoopGroup group(4);

fiber::http::LocalHttp1ConnectionPoolSet::Options options{};
options.max_idle_per_group = 2;
options.max_idle_total = 64;
options.initial_group_capacity = 32;

fiber::http::LocalHttp1ConnectionPoolSet pool_set(group, options);
if (!pool_set.init()) {
    // 处理初始化失败
}
```

### 6.3 使用方式

`LocalHttp1ConnectionPoolSet` 会根据当前线程绑定的 `EventLoop::current()` 自动路由到当前 loop 对应的 core。

```cpp
fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
    auto lease = pool_set.acquire(key);

    if (!lease.hit()) {
        auto conn_result = lease.emplace_connection(client_options);
        if (!conn_result) {
            co_return;
        }

        auto connect_result = co_await lease.connection().connect(std::chrono::seconds(5));
        if (!connect_result) {
            co_return;
        }
    }

    // 使用 lease.connection()
    lease.reset();
});
```

### 6.4 行为特点

- 本地命中：直接返回已有 idle 连接
- 本地 miss：返回当前 loop 的 miss lease
- 不会探测其它 loop
- `idle_total()` / `group_count()` 返回的是当前 loop 对应 shard 的统计，不是全局总数

### 6.5 `sweep_expired()`

`LocalHttp1ConnectionPoolSet::sweep_expired(now)` 只清理当前 loop shard。

如果你需要每个 loop 主动清理超时连接，应当在每个 loop 上各自调用。

### 6.6 `clear()` 与 `shutdown()`

`LocalHttp1ConnectionPoolSet` 只提供异步管理接口：

- `clear_async()`
- `shutdown_async()`

它们都会把操作分发到每个 shard 的 owner loop 执行，因此需要在协程里 `co_await`。

区别是：

- `clear_async()` 只清当前 idle 连接
- `shutdown_async()` 会让整个 set 进入不可再复用、不可再建连的状态

## 7. `StealableHttp1ConnectionPoolSet`

### 7.1 适用场景

选择 `StealableHttp1ConnectionPoolSet` 的典型场景：

- 多个 loop 会访问同一个上游地址组
- 你希望减少总连接数
- 愿意接受本地 miss 时的一次跨 loop idle 连接借用

### 7.2 初始化

```cpp
fiber::event::EventLoopGroup group(4);

fiber::http::StealableHttp1ConnectionPoolSet::Options options{};
options.max_idle_per_group = 2;
options.max_idle_total = 64;
options.initial_group_capacity = 32;

fiber::http::StealableHttp1ConnectionPoolSet pool_set(group, options);
if (!pool_set.init()) {
    // 处理初始化失败
}
```

### 7.3 acquire 是异步的

`StealableHttp1ConnectionPoolSet::acquire()` 返回 awaiter，需要 `co_await`：

```cpp
auto lease = co_await pool_set.acquire(key);
```

原因是：

- 本地 miss 后，可能需要向其它 loop 提交 steal 请求
- steal 流程通过 `EventLoop::post()` 串行推进

本地 hit 和没有远端 hint 候选的 local miss 都同步完成，不进行动态分配。只有确实需要跨 loop
探测时，awaiter 才会分配一个独立的 acquire state。跨线程 `NotifyEntry` 位于这个 state 中，
因此等待协程被取消后，已经投递的通知仍有稳定存储可用于完成清理。

### 7.4 steal 流程

当前实现流程：

1. 先查当前 loop 对应 shard 的 `Http1ConnectionPoolCore`
2. 如果本地 hit，立即返回，不挂起协程
3. 如果本地 miss，保留一个当前 loop 的 miss lease 作为 fallback
4. 从当前 loop 的下一个 shard 开始，沿环形链表依次向后探测
5. 先读取该 shard 的 `Http1ConnectionGroupHintTable`
6. 只有 `approx_count > 0` 时，才真正向那个 loop 提交 steal 请求
7. 某个远端命中后，返回 remote borrowed lease
8. 如果所有其它 loop 都没有候选或 steal 失败，返回最开始保留的本地 miss lease

重要点：

- cheap hint 只是剪枝，不保证远端一定能 steal 到连接
- 真正的“检查 + 摘走 idle 连接”在远端 loop 的 core 内完成
- 如果某个远端 loop 在 hint 命中但实际 steal miss，会继续向后探测后续 loop

跨 loop acquire state 使用原子状态仲裁完成和取消。awaiter 析构只把 state 标记为 canceled，
不会尝试从 MPSC notify queue 中删除节点。后续 callback 看到 canceled 后不会再访问 awaiter 或恢复
原协程；如果连接已经从远端 pool 摘下，则先在它的 home loop 归还，再释放 state。因此
`StealableHttp1ConnectionPoolSet::AcquireAwaiter` 可以直接作为 `when_any()` 分支，也可以安全地位于
被 `Task::select()` 取消的子协程 frame 中。

### 7.5 使用方式

```cpp
fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
    auto lease = co_await pool_set.acquire(key);

    if (!lease.hit()) {
        auto conn_result = lease.emplace_connection(client_options);
        if (!conn_result) {
            co_return;
        }

        auto connect_result = co_await lease.connection().connect(std::chrono::seconds(5));
        if (!connect_result) {
            co_return;
        }
    }

    auto &conn = lease.connection();
    // 发请求

    lease.reset();
});
```

### 7.6 lease 的两种来源

`StealableHttp1ConnectionPoolSet::Lease` 有两种来源。

#### 7.6.1 Local lease

本地 hit 或本地 miss fallback。

特点：

- 语义和 `Http1ConnectionPoolCore::Lease` 一致
- miss 时可以 `emplace_connection(...)`
- 新建连接绑定到当前 loop

#### 7.6.2 Remote borrowed lease

从其它 loop steal 到一条已有 idle 连接。

特点：

- `hit() == true`
- `has_connection() == true`
- 不允许 `emplace_connection(...)`
- `lease.connection().loop()` 是 home loop，不一定是当前 loop
- `reset()` 时会自动归还到 home loop 的 core

### 7.7 return 行为

remote borrowed 连接释放时：

- 如果当前正好就在 home loop，直接回到 home core
- 否则通过 entry 内嵌的 `NotifyEntry` 投递回 home loop

当前实现是“借用”，不是“迁移”：

- 远端连接不会转移到当前 loop 的本地池
- 用完后仍然回原始 home loop

### 7.8 hint 表

每个 shard 维护一张 `Http1ConnectionGroupHintTable`。

作用：

- 回答“这个 loop 大概率有没有这个 `GroupKey` 的 idle 连接”

特点：

- 单写多读
- 近似计数
- 允许 false positive
- 不作为正确性依据

### 7.9 `clear_async()`

`StealableHttp1ConnectionPoolSet::clear_async()` 会清所有 shard 当前 idle 连接，并清空所有 hint 表。

它不会主动取消当前已经借出的 lease。

因此仍然要保证：

- pool set 生命周期长于所有 lease

如果某条连接在 `clear_async()` 时正处于 borrowed 状态，那么它归还时仍然会回到 home loop。

### 7.10 `shutdown_async()`

`StealableHttp1ConnectionPoolSet::shutdown_async()` 会：

- 禁止注册新的跨 loop acquire state，并等待已有 state 完成或取消清理
- 在每个 shard 的 owner loop 上清空当前 idle 连接
- 清空所有 hint 表
- 禁止后续新的 `acquire()` / `emplace_connection()`
- 让 borrowed 连接在归还时直接销毁，而不是重新回池

## 8. local 和 steal 怎么选

### 8.1 选择 `LocalHttp1ConnectionPoolSet`

适合：

- 更关注本地命中和 miss 的响应速度
- 不在乎不同 loop 上会多建一些连接
- 调用路径想尽量简单

特点：

- `acquire()` 同步返回
- 不会跨 loop 探测
- 没有 hint 维护开销

### 8.2 选择 `StealableHttp1ConnectionPoolSet`

适合：

- 同组请求会在多个 loop 分布
- 希望尽量少建连接
- 接受本地 miss 时一次跨 loop idle steal

特点：

- `acquire()` 需要 `co_await`
- 本地 miss 时会读 hint，并可能跨 loop steal
- 命中远端 idle 连接后，减少新建连接数量

## 9. 最小示例

### 9.1 local 模式

```cpp
fiber::event::EventLoopGroup group(2);
fiber::http::LocalHttp1ConnectionPoolSet pool_set(group);
FIBER_ASSERT(pool_set.init());

group.start();
fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
    auto key = fiber::http::Http1ConnectionGroupKey::from_ip(
        fiber::net::IpAddress::loopback_v4(),
        8080,
        fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    auto lease = pool_set.acquire(key);
    if (!lease.hit()) {
        fiber::http::Http1ClientConnectionOptions options;
        options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 8080);

        auto conn_result = lease.emplace_connection(options);
        if (!conn_result) {
            co_return;
        }

        auto connect_result = co_await lease.connection().connect();
        if (!connect_result) {
            co_return;
        }
    }

    lease.reset();
});
```

### 9.2 steal 模式

```cpp
fiber::event::EventLoopGroup group(2);
fiber::http::StealableHttp1ConnectionPoolSet pool_set(group);
FIBER_ASSERT(pool_set.init());

group.start();
fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
    auto key = fiber::http::Http1ConnectionGroupKey::from_ip(
        fiber::net::IpAddress::loopback_v4(),
        8080,
        fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    auto lease = co_await pool_set.acquire(key);
    if (!lease.hit()) {
        fiber::http::Http1ClientConnectionOptions options;
        options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 8080);

        auto conn_result = lease.emplace_connection(options);
        if (!conn_result) {
            co_return;
        }

        auto connect_result = co_await lease.connection().connect();
        if (!connect_result) {
            co_return;
        }
    }

    lease.reset();
});
```

## 10. 常见误用

- 在不属于 pool 的 `EventLoop` 上直接使用 `Http1ConnectionPoolCore`
- `StealableHttp1ConnectionPoolSet` 上忘记 `co_await acquire(...)`
- 对 remote borrowed lease 调用 `emplace_connection(...)`
- 用 HTTPS 请求却把 `http` 和 `https` 混成同一个 `GroupKey`
- pool 或 pool set 先析构，lease 还活着

## 11. 测试参考

当前行为可参考这些测试：

- `tests/Http1ConnectionPoolTest.cpp`
- `tests/LocalHttp1ConnectionPoolSetTest.cpp`
- `tests/StealableHttp1ConnectionPoolSetTest.cpp`
- `tests/Http1ConnectionGroupKeyTest.cpp`
- `tests/Http1ConnectionGroupHintTableTest.cpp`
