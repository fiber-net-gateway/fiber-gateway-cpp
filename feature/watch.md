# Coroutine Watch<T>（单 Publisher、多 Subscriber）

## 目标

- 提供只保留最新状态的异步通知组件，不提供消息队列或历史事件语义。
- 一个 `Watch<T>` 生命周期内最多创建一个 `Publisher`。
- 支持任意数量的 `Subscriber`，消费进度由调用方通过版本号显式管理。
- 支持通过 `co_await subscriber.next(received_version)` 异步等待新版本。
- 同时返回不可变值和与其原子对应的版本号。
- `publish()` 可以从其它线程调用，Subscriber 必须回到发起等待的 `EventLoop` 恢复。
- 发布、读取和等待注册之间不存在丢失唤醒。
- 等待协程销毁或超时时，可以安全取消等待节点。

适用场景包括配置刷新、服务发现、连接状态、Leader 状态和运行时参数同步。

## 非目标

- 不保存历史版本，也不保证 Subscriber 观察到每一次 `publish()`。
- 不提供消息队列、背压或逐消息确认。
- 不提供 `close()` 或 Publisher 销毁通知。
- 不提供线程阻塞式等待接口。

## Public API

实现位于 `include/fiber/async/Watch.h`，模板代码全部放在头文件中。

```cpp
namespace fiber::async {

template<typename T>
class Watch {
public:
    struct Snapshot {
        std::shared_ptr<const T> value;
        std::uint64_t version = 0;
    };

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

    [[nodiscard]] Snapshot current() const;
    [[nodiscard]] NextAwaiter next(std::uint64_t received_version) const noexcept;
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

`Subscriber` 不保存消费版本。调用方持有最后已经处理的
`received_version`：

- `current()` 返回最新的 `{value, version}`，没有消费游标副作用。
- `next(0)` 在空 Watch 上等待第一次发布。
- 当前版本大于 `received_version` 时，`next()` 立即返回最新快照。
- 当前版本等于 `received_version` 时，`next()` 等待后续发布。
- `received_version` 大于当前版本属于调用方游标错误，通过 `FIBER_ASSERT` 检查。
- `next()` 恢复时读取当时的最新快照，而不是触发唤醒的那个历史快照。

例如：

```text
Caller received_version = 0

publish(A)  // version 1，唤醒 Subscriber
publish(B)  // version 2
publish(C)  // version 3

next(0) 恢复并返回 {C, 3}
Caller 将 received_version 更新为 3
next(3) 等待 version > 3
```

`std::shared_ptr<const T>` 保证快照内容不可通过 Watch API 修改。Subscriber 可以继续持有旧快照；Watch 自身始终只保存最新快照，这不构成历史队列。

`next()` 不提供默认版本参数，避免循环重复调用 `next()` 时不断以版本
`0` 立即取得同一个已发布快照。

## 生命周期

`Watch`、`Publisher` 和每个 `Subscriber` 都通过 `std::shared_ptr<SharedState>` 持有共享状态：

- Watch 销毁后，已创建的 Publisher 和 Subscriber 仍然有效。
- Publisher 销毁不产生通知、不清空当前值，也不允许重新获取 Publisher。
- Subscriber 销毁不影响 Publisher 或其它 Subscriber。
- 最后一个句柄和等待 Awaiter 释放后，共享状态才被销毁。

Publisher 和 Subscriber 都是 move-only 对象。`NextAwaiter` 独立持有共享状态，
不借用 Subscriber；创建 Awaiter 后移动或销毁 Subscriber 不影响该等待。
Awaiter 析构会取消等待，因此正常销毁协程帧是安全的。

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

Waiter
  loop: EventLoop*
  handle: coroutine_handle<>
  state: atomic<Waiting | Notified | Resumed | Canceled>
  prev/next: Waiter*
  queued: bool
  notify_entry: EventLoop::NotifyEntry
  defer_entry: EventLoop::DeferEntry
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
7. 根据每个 Waiter 捕获的 owner loop 投递恢复回调：同 loop 使用 local defer queue，
   跨 loop 使用 notify queue。

发布路径不使用 `std::vector` 或 `std::function`。摘除链表后直接遍历原有 intrusive links，避免额外的批量通知容器分配。

协程不会在 `publish()` 调用栈中直接恢复。同一 EventLoop 使用 local defer queue，避免
publish 发生在 notify drain 之后时，因为没有额外 wakeup 而滞留到下一次外部事件；跨
EventLoop 仍通过 notify queue 唤醒 owner loop。

## NextAwaiter 算法

### await_ready

在共享锁内比较 `SharedState::version` 和调用方传入的
`received_version`。当前版本更大时立即完成，不分配 Waiter；传入未来
版本时触发断言。

### await_suspend

1. 要求当前线程已经安装 `EventLoop`。
2. 创建一个堆上 Waiter，保存 coroutine handle 和当前 EventLoop。
3. 在共享锁内再次比较当前版本和 `received_version`。
4. 如果当前版本更大，释放 Waiter 并返回 `false`。
5. 如果版本相等，将 Waiter 加入链表并挂起协程。

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

在共享锁内读取最新值和版本，确认版本严格大于 `received_version`，然后
返回 `Snapshot`。消费进度由调用方在处理成功后更新。

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

- `subscribe()` 只复制共享状态句柄；`acquire_publisher()` 的唯一性检查由
  共享锁保护，两者都可以与其它共享状态操作并发。
- `publish()` 可以从任意线程调用，通知会返回各 Subscriber 的原始 EventLoop。
- 唯一 Publisher 可以在线程之间移动，但调用者不应并发重叠调用同一个 Publisher 的 `publish()`。
- 不同 Subscriber 可以在不同线程和 EventLoop 中独立使用。
- 同一个 Subscriber 可以创建多个独立 Awaiter；每个 Awaiter 使用自己
  捕获的 `received_version`。
- 调用方负责避免错误地回退或混用其它 Watch 的版本号。
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
        [&subscriber, received_version]() { return subscriber.next(received_version); },
        std::chrono::seconds(5));

if (!result) {
    co_return;
}

received_version = result->version;
std::shared_ptr<const Config> config = std::move(result->value);
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
4. `current()` 同时返回不可变值和版本，且不推进隐藏游标。
5. `next(0)` 可以立即取得已经存在的初始值或最新发布值。
6. 多次发布只返回最新合并值和对应版本。
7. 已挂起的 `next(version)` 在发布后恢复，并合并恢复前的连续发布。
8. 同一个 Subscriber 可以按显式版本创建独立等待。
9. 多 EventLoop Subscriber 回到各自线程恢复。
10. 同 loop 的 deferred callback 发布时无需外部 wakeup 也能恢复 waiter。
10. 销毁挂起协程会取消 Waiter，不会被后续发布恢复。
11. `timeout_for()` 后可以继续使用原版本等待。

验证命令：

```bash
cmake --build build --target fiber_tests -j2
./build/fiber_tests --gtest_filter=WatchTest.*
ctest --test-dir build --output-on-failure -j2
```
