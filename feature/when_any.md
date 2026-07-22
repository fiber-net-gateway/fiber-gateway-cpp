# Coroutine `when_any`

## 目标

`fiber::async::when_any()` 同时等待两个及以上 Awaiter，第一个完成的分支恢复调用协程并提供结果。组合对象不为每个分支创建辅助协程，也不要求分支 Awaiter 可复制或可移动。

实现位于：

- `src/async/Awaitable.h`：通用 Awaiter 检测和 `SelectableAwaiter` 协议。
- `src/async/TaskSelect.h`：拥有 Task coroutine frame 的 `TaskSelectAwaiter<T>`。
- `src/async/WhenAny.h`：`WhenAnyAwaiter`、`WhenAnyResult` 和 `when_any()`。

## Public API

```cpp
template<typename T>
concept SelectableAwaiter = /* awaiter + completed/cancellation contract */;

template<typename... Results>
class WhenAnyResult {
public:
    [[nodiscard]] std::size_t index() const noexcept;

    template<std::size_t Index>
    [[nodiscard]] bool is() const noexcept;

    template<std::size_t Index>
    decltype(auto) get() &;

    template<std::size_t Index>
    decltype(auto) get() const &;

    template<std::size_t Index>
    decltype(auto) get() &&;
};

template<typename... Awaiters>
class WhenAnyAwaiter;

template<typename... Factories>
[[nodiscard]] auto when_any(Factories &&...factories);
```

`when_any()` 接收至少两个无参 Factory，而不是已经构造好的 Awaiter：

```cpp
using namespace std::chrono_literals;

auto result = co_await fiber::async::when_any(
        [&]() { return mutex.lock(); },
        []() { return fiber::async::sleep(100ms); });

if (result.is<0>()) {
    auto guard = std::move(result).get<0>();
    // 持有 mutex
} else {
    result.get<1>(); // void 分支
    // 超时
}
```

Factory 必须返回 prvalue `SelectableAwaiter`。每个 Factory 的调用结果直接初始化 `WhenAnyAwaiter` 内对应的成员，没有中间 tuple，也不需要 Awaiter 的移动构造函数。Factory 自身只在构造组合对象期间被借用。

`Task<T>` 通过右值限定的 `select()` 转换为可选择 Awaiter：

```cpp
#include "async/TaskSelect.h"

auto result = co_await fiber::async::when_any(
        []() { return request_task().select(); },
        []() { return fiber::async::sleep(100ms); });
```

`select()` 把 coroutine handle 从 Task 转移给不可移动的 `TaskSelectAwaiter<T>`。原 Task 随即变为空对象，TaskSelectAwaiter 独占并最终销毁该 coroutine frame。左值 Task 必须显式 `std::move(task).select()`，避免无意转移所有权。

## 返回值

`WhenAnyResult<R0, R1, ...>` 是按索引区分的 tagged variant：

- `index()` 返回获胜分支的从零开始索引。
- `is<I>()` 判断分支 `I` 是否获胜。
- `get<I>()` 只能用于获胜分支，索引错误会触发 `FIBER_ASSERT`。
- 值结果存入 Result 并支持 move-only 类型。
- 左值引用结果以 `std::reference_wrapper` 保存，`get<I>()` 仍返回引用。
- `void` 结果不保存值，`get<I>()` 只校验分支索引。
- 即使多个分支的返回类型相同，也始终通过索引准确区分。

只有获胜分支会执行 `await_resume()`。其余 Awaiter 不会产生结果，并在组合 Awaiter 的 `await_resume()` 返回前析构取消。

## `SelectableAwaiter` 协议

除标准 Awaiter 接口外，可选择 Awaiter 必须满足：

```cpp
bool completed() const noexcept;
```

并满足以下生命周期约束：

1. `await_suspend()` 的返回类型必须严格为 `void` 或 `bool`。返回 coroutine handle 的对称转移 Awaiter 不在当前版本支持范围内。
2. 析构函数必须为 `noexcept`。如果 Awaiter 已注册定时器、事件节点或回调，析构必须取消或解除它们，保证析构后不会再恢复原协程，也不会再访问 Awaiter。
3. `await_ready()` 返回 `true` 前必须先令 `completed()` 返回 `true`。
4. `await_suspend()` 返回 `false` 前必须先令 `completed()` 返回 `true`。
5. 异步完成时，必须在所属 EventLoop 的恢复回调中先令 `completed()` 返回 `true`，再恢复协程。
6. `await_suspend()` 返回 `true` 或 `void` 时，不允许在 `await_suspend()` 调用栈中内联恢复协程。
7. 同一个组合中的所有异步恢复必须串行回到创建等待的 owner EventLoop。跨线程生产者只负责投递，不直接恢复协程。

第 2、5、6、7 项是语义约束，C++ concept 无法完全在编译期验证。违反约束会产生悬空回调或重复恢复。

当前已满足协议的内置 Awaiter 包括 `SleepAwaiter`、`YieldAwaiter`、`WaitGroup::JoinAwaiter`、`Watch<T>::Subscriber::NextAwaiter`、`Mutex::LockAwaiter`、RWMutex 的读写 Awaiter、`SignalAwaiter` 和 `TaskSelectAwaiter<T>`。`TimeoutAwaiter` 在内部 Awaiter 也满足协议时可选择。

普通 `Task<T>::Awaiter` 仍不满足协议：它的 `await_suspend()` 返回 coroutine handle 进行对称转移，而且 Awaiter 自身不拥有 Task frame。普通 `co_await task()` 保留这条对称转移快路径；只有参与 `when_any()` 时才使用拥有 frame 的 `task().select()`。

`TaskSelectAwaiter<T>` 的 `await_suspend()` 不直接启动 lazy Task，而是把首次 `resume()` 作为 intrusive defer entry 投递到 owner EventLoop。这样组合可以先完成所有分支的 arming 并从 `await_suspend()` 返回；即使 Task 不发生内部挂起就直接完成，也不会在组合的 arming 调用栈中恢复父协程。

Task 到达 `final_suspend` 后，`handle.done()` 在对称转移回父协程前已经为 true，因此 `TaskSelectAwaiter::completed()` 可以直接查询 coroutine handle，不需要额外完成标记。Task loser 析构时：

- 尚未启动：取消 defer entry，再销毁 frame；
- 已启动并挂起：销毁 frame，由 frame 内当前 Awaiter 的析构取消底层等待；
- 已完成：在提取结果后销毁 final-suspended frame。

被选择的 Task 内部同样必须遵守 owner EventLoop 恢复和 Awaiter 析构取消约束。Task 启动后仍可创建其自身固有的 coroutine frame；`when_any()` 不会为它再创建协调 coroutine frame。

## 调度算法

### `await_ready()`

组合按参数顺序调用各分支的 `await_ready()`。第一个返回 `true` 的分支立即获胜，后续分支不再探测，因此多个同步就绪分支具有稳定的左优先级。

### `await_suspend()`

组合按参数顺序把同一个调用协程 handle 注册给各分支：

- `void await_suspend(...)` 表示分支已挂起。
- `bool await_suspend(...) == true` 表示分支已挂起。
- `bool await_suspend(...) == false` 表示该分支同步获胜，组合停止注册后续分支并返回 `false`。

如果后面的分支同步获胜，前面已挂起的分支在组合的 `await_resume()` 中统一析构取消，不需要单独的回滚容器。

### 异步恢复与 `await_resume()`

异步分支在 owner EventLoop 回调中先设置自己的完成标记，再恢复共享的调用协程。组合的 `await_resume()` 扫描 `completed()` 找到唯一获胜分支，只调用该分支的 `await_resume()`，构造带索引结果。

调用协程恢复后，组合先确定唯一 winner，再立即析构所有 loser，最后只调用 winner 的 `await_resume()`。loser 的析构会取消尚未执行的回调。由于回调在同一个 EventLoop 串行执行，第一个回调恢复协程并销毁 loser 后，其它分支不能再次恢复该协程。winner 随组合对象正常销毁。

`WhenAnyAwaiter` 自身也实现 `completed()` 并满足相同协议，因此 `when_any()` 可以嵌套。

## 内存与协程帧

`WhenAnyAwaiter` 通过按索引展开的成员存储内联持有每个 Awaiter。组合自身：

- 不创建辅助 `Task`；
- 不创建分支协程帧；
- 不使用 `std::function`；
- 不为分支列表进行动态分配。

当调用协程挂起时，编译器会把组合 Awaiter 保存在调用协程已有的 frame 中。具体分支 Awaiter 仍可按自身实现分配等待节点，例如 Mutex 和 Watch 的 waiter 节点；这不属于组合层的额外协程 frame。

## 限制

- 至少需要两个分支。
- 当前不接受裸 Awaiter 参数，必须使用 Factory，以保留非移动 Awaiter 的直接构造保证。
- 不支持返回 coroutine handle 的 `await_suspend()`。
- Task 必须显式转换为 `task().select()`；普通 `Task::Awaiter` 的对称转移接口不能直接作为分支。
- 不提供跨 EventLoop 并发恢复仲裁；分支必须遵守 owner EventLoop 串行恢复协议。
- loser 不调用 `await_resume()`，资源清理由 Awaiter 析构完成。
