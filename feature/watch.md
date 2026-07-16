# Coroutine Watch<T>（单 Publisher、多 Subscriber）

## 目标

- 提供只保留最新状态的异步通知组件，不提供消息队列或历史事件语义。
- 一个 `Watch<T>` 生命周期内最多创建一个 `Publisher`。
- 支持任意数量、观察进度相互独立的 `Subscriber`。
- 支持通过 `co_await subscriber.next()` 异步等待新版本。
- `publish()` 可以从其它线程调用，Subscriber 必须回到发起等待的 `EventLoop` 恢复。
- 发布、读取和等待注册之间不存在丢失唤醒。
- 等待协程销毁或超时时，可以安全取消等待节点。

适用场景包括配置刷新、服务发现、连接状态、Leader 状态和运行时参数同步。

## 非目标

- 不保存历史版本，也不保证 Subscriber 观察到每一次 `publish()`。
- 不提供消息队列、背压或逐消息确认。
- 不提供 `close()` 或 Publisher 销毁通知。
- 不允许同一个 Subscriber 同时执行多个 `next()`。
- 不提供线程阻塞式等待接口。

## Public API

实现位于 `src/async/Watch.h`，模板代码全部放在头文件中。

```cpp
namespace fiber::async {

template<typename T>
class Watch {
public:
    class Publisher;
    class Subscriber;

    Watch();
    explicit Watch(T initial_value);

    Watch(const Watch &) = delete;
    Watch &operator=(const Watch &) = delete;
    Watch(Watch &&) noexcept;
    Watch &operator=(Watch &&) noexcept;

    [[nodiscard]] std::optional<Publisher> acquire_publisher() noexcept;
    [[nodiscard]] Subscriber subscribe();
};

template<typename T>
class Watch<T>::Publisher {
public:
    Publisher(const Publisher &) = delete;
    Publisher &operator=(const Publisher &) = delete;
    Publisher(Publisher &&) noexcept;
    Publisher &operator=(Publisher &&) noexcept;

    void publish(T value);
};

template<typename T>
class Watch<T>::Subscriber {
public:
    class NextAwaiter;

    Subscriber(const Subscriber &) = delete;
    Subscriber &operator=(const Subscriber &) = delete;
    Subscriber(Subscriber &&) noexcept;
    Subscriber &operator=(Subscriber &&) noexcept;

    [[nodiscard]] std::shared_ptr<const T> current();
    [[nodiscard]] NextAwaiter next() noexcept;
};

} // namespace fiber::async
```

`acquire_publisher()` 使用 `std::optional` 表达唯一 Publisher 已被获取。项目不使用异常，因此重复获取不通过抛出异常报告失败。

## 最新值和版本语义

共享状态使用单调递增的 `std::uint64_t version` 标识发布版本：

- 空 Watch 的版本为 `0`，当前值为 `nullptr`。
- 带初始值的 Watch 从版本 `1` 开始。
- 每次 `publish()` 都递增版本，即使新旧 `T` 内容相同。
- 不要求 `T` 提供相等比较。
- 版本溢出是不可恢复的使用周期错误，通过 `FIBER_ASSERT` 检查。

每个 Subscriber 保存自己的 `observed_version`：

- `subscribe()` 将创建时的当前版本设为已观察基线。
- `current()` 返回最新快照，并将该快照版本标记为已观察。
- `next()` 只等待严格晚于 `observed_version` 的版本。
- `next()` 恢复时读取当时的最新快照，而不是触发唤醒的那个历史快照。

例如：

```text
Subscriber observed_version = 0

publish(A)  // version 1，唤醒 Subscriber
publish(B)  // version 2
publish(C)  // version 3

Subscriber 恢复并读取 version 3，只观察到 C。
```

`std::shared_ptr<const T>` 保证快照内容不可通过 Watch API 修改。Subscriber 可以继续持有旧快照；Watch 自身始终只保存最新快照，这不构成历史队列。

## 生命周期

`Watch`、`Publisher` 和每个 `Subscriber` 都通过 `std::shared_ptr<SharedState>` 持有共享状态：

- Watch 销毁后，已创建的 Publisher 和 Subscriber 仍然有效。
- Publisher 销毁不产生通知、不清空当前值，也不允许重新获取 Publisher。
- Subscriber 销毁不影响 Publisher 或其它 Subscriber。
- 最后一个句柄和等待 Awaiter 释放后，共享状态才被销毁。

Publisher 和 Subscriber 都是 move-only 对象。Subscriber 存在活动 `next()` 时禁止移动或销毁；Awaiter 析构会先取消等待，因此正常销毁协程帧是安全的。

## 数据模型

```text
Watch<T>::SharedState
  mutex: std::mutex
  latest: shared_ptr<const T>
  version: uint64_t
  publisher_acquired: bool
  waiters_head/waiters_tail: Waiter*

Subscriber
  state: shared_ptr<SharedState>
  observed_version: uint64_t
  next_active: bool

Waiter
  loop: EventLoop*
  handle: coroutine_handle<>
  state: atomic<Waiting | Notified | Resumed | Canceled>
  prev/next: Waiter*
  queued: bool
  notify_entry: EventLoop::NotifyEntry
```

共享锁同时保护 `latest`、`version`、Publisher 获取标记和等待链表。值与版本必须在同一个临界区读取，避免观察到不匹配的快照。

等待链表只包含当前真正阻塞的 Subscriber，不为所有 Subscriber 建立常驻注册节点。

## Publisher 获取

`acquire_publisher()` 在共享锁内检查并设置 `publisher_acquired`：

```text
false -> true：返回 Publisher
true  -> true：返回 nullopt
```

标记不会在 Publisher 析构时重置，从而保证整个共享状态生命周期内最多创建一个 Publisher。

## Publish 算法

`Publisher::publish(T value)` 首先在锁外创建新的共享快照，然后调用共享状态发布：

1. 获取共享锁。
2. 将新快照与 `latest` 交换并递增 `version`。
3. 整体摘除等待链表。
4. 将摘除节点从 `Waiting` 标记为 `Notified`。
5. 释放共享锁。
6. 让旧快照在锁外释放。
7. 通过每个 Waiter 捕获的 `EventLoop::post()` 投递恢复回调。

发布路径不使用 `std::vector` 或 `std::function`。摘除链表后直接遍历原有 intrusive links，避免额外的批量通知容器分配。

协程不会在 `publish()` 调用栈中直接恢复。即使 Publisher 与 Subscriber 位于同一 EventLoop，也通过 notify queue 异步恢复。

## NextAwaiter 算法

### await_ready

在共享锁内比较 `SharedState::version` 和 Subscriber 的 `observed_version`。存在新版本时立即完成，不分配 Waiter。

### await_suspend

1. 要求当前线程已经安装 `EventLoop`。
2. 创建一个堆上 Waiter，保存 coroutine handle 和当前 EventLoop。
3. 在共享锁内再次比较版本。
4. 如果版本已变化，释放 Waiter 并返回 `false`。
5. 如果版本未变化，将 Waiter 加入链表并挂起协程。

第二次版本检查用于关闭以下丢失唤醒窗口：

```text
await_ready() 观察到无变化
             |
             +-- publish(new_value)
             |
await_suspend() 尝试入队
```

如果发布发生在两次检查之间，`await_suspend()` 不入队也不挂起，随后 `await_resume()` 直接读取最新值。

### await_resume

在共享锁内读取最新值和版本，将版本写入 Subscriber 的 `observed_version`，然后返回 `std::shared_ptr<const T>`。

发布通知与实际恢复之间可以发生更多发布；`await_resume()` 总是返回恢复时的最新状态，从而自然实现更新合并。

## Waiter 状态机与取消

```text
Waiting --publish--> Notified --EventLoop callback--> Resumed
   |                     |
   +------cancel---------+--------------------------> Canceled
```

- `Waiting -> Canceled`：在共享锁内从链表移除、清空 handle，并立即释放 Waiter。
- `Notified -> Canceled`：恢复回调已经或即将投递，只清空 handle；回调执行时不恢复协程并负责释放 Waiter。
- `Notified -> Resumed`：回调通过原子 CAS 获得唯一恢复权，随后恢复 handle 并释放 Waiter。
- `Resumed`：正常 `await_resume()` 已清除 Awaiter 内的 Waiter 指针，不再执行取消。

这一模型允许协程帧销毁和 `timeout_for()` 安全取消等待，不会发生双重恢复或双重释放。

## 线程规则

- `subscribe()` 和 `acquire_publisher()` 由共享锁保护，可以与其它共享状态操作并发。
- `publish()` 可以从任意线程调用，通知会返回各 Subscriber 的原始 EventLoop。
- 唯一 Publisher 可以在线程之间移动，但调用者不应并发重叠调用同一个 Publisher 的 `publish()`。
- 不同 Subscriber 可以在不同线程和 EventLoop 中独立使用。
- 同一个 Subscriber 必须由一个协程串行使用。
- 同一个 Subscriber 最多允许一个活动 `next()`；违反时触发 `FIBER_ASSERT`。
- `next()` 只能在 EventLoop 协程内调用；`current()` 是同步非阻塞接口。

## Shutdown 语义

Publisher 不提供 `close()`，Publisher 析构也不唤醒等待者。因此没有后续发布时，`next()` 可以永久等待。

调用方关闭服务时必须选择以下方式之一：

- 销毁拥有等待协程的任务，让 Awaiter 析构取消 Waiter。
- 使用 `timeout_for()` 为等待设置超时。
- 在更高层通过其它关闭信号结束观察循环。

超时示例：

```cpp
auto result = co_await fiber::async::timeout_for(
        [&subscriber]() { return subscriber.next(); },
        std::chrono::seconds(5));

if (!result) {
    co_return;
}

std::shared_ptr<const Config> config = std::move(*result);
```

使用 factory 形式可以直接构造不可移动的 `NextAwaiter`。

## 性能

```text
操作                         时间复杂度       动态分配
subscribe()                  O(1)             无
current()                    O(1)             无，仅 shared_ptr 引用计数
next() 已有新版本            O(1)             无
next() 实际挂起              O(1)             一个 Waiter
publish()                    O(W)             一个 T/control block
取消 Waiting Waiter          O(1)             无额外分配
```

`W` 是发布时真正阻塞的 Subscriber 数量，而不是 Subscriber 总数。每个等待者都必须被投递一次，因此发布的 `O(W)` 通知成本不可避免。

Waiter 使用独立堆对象，是因为跨线程 `EventLoop::NotifyEntry` 一旦投递便不能从 MPSC 队列撤销。取消后仍需要一个独立存活的节点承接回调并完成释放。

## 测试

测试位于 `tests/WatchTest.cpp`，覆盖：

1. 空状态和初始值的 `current()`。
2. Publisher 只能获取一次，销毁后也不能重新获取。
3. Publisher 和 Subscriber 可以超过 Watch 对象生命周期。
4. 多次发布只返回最新合并值。
5. 已挂起的 `next()` 在发布后恢复，并合并恢复前的连续发布。
6. 不同 Subscriber 的观察版本相互独立。
7. 多 EventLoop Subscriber 回到各自线程恢复。
8. 销毁挂起协程会取消 Waiter，不会被后续发布恢复。
9. `timeout_for()` 后 Subscriber 可以继续等待下一版本。

验证命令：

```bash
cmake --build build --target fiber_tests -j2
./build/fiber_tests --gtest_filter=WatchTest.*
ctest --test-dir build --output-on-failure -j2
```
