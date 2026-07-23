# `src/log` 实现评估与重构建议

评估日期：2026-07-18

## 1. 总体评价

当前实现的基础架构合理，不需要推倒重写。Logger 路由在初始化阶段展开，禁用日志不会构造消息，Appender 地址在运行期保持稳定，文件滚动受到锁保护，线程本地 buffer 也避免了常见热路径上的动态分配。现有实现的主要风险集中在 LogContext 与 EventLoop 的生命周期耦合，以及启用日志热路径上的固定成本。

截至本次评估，`fiber_lib` 和 `fiber_tests` 均可成功构建，现有 18 个日志相关测试全部通过。现有测试说明路由、截断、buffer、reopen、文件滚动和 console 并发完整写入的主流程稳定，但尚未覆盖下文列出的部分边界。

## 2. 高优先级问题

### 2.1 ConsoleAppender 完整写入（已解决）

处理状态：已完成整改。ConsoleAppender 现在用进程内共享互斥保护 stdout/stderr 的完整记录，并循环处理
`EINTR` 和短写；部分写入失败会分别统计已写字节和被丢弃记录。

整改前，[`ConsoleAppender::append()`](../src/log/Appender.cpp) 对 stdout/stderr 只调用一次 `write()`，没有
重试 `EINTR` 或短写，也没有在多个产生日志的线程之间串行化完整记录。这在 stdout/stderr 指向管道时
尤其明显：最大格式化日志行是 9216 字节，超过 Linux 常见的 4096 字节 `PIPE_BUF`，多个线程写长日志时
可能互相交错。

新增的 pipe 并发测试让多个线程写入超过 `PIPE_BUF` 的记录，并逐条校验 marker、payload 和换行边界。

### 2.2 LogContext 保存裸 EventLoop 指针，存在生命周期风险

[`LogContext::attach_loop()`](../src/log/LogContext.cpp#L50) 将 `EventLoop *` 长期保存在 LogContext 内，随后 [`detach_loop()`](../src/log/LogContext.cpp#L69) 会通过该指针查询状态并取消 timer。如果局部 EventLoop 已经析构，而线程本地 LogContext 尚未 reset，后续 shutdown、重新绑定另一个 EventLoop 或线程退出都可能访问失效对象。

lite_nginx 当前先声明 logging shutdown guard，之后才声明主 EventLoop，因此析构顺序是 EventLoop 在前、logging shutdown 在后。只有在主 EventLoop 内实际产生日志且运行时配置了 buffered appender 时才会触发该风险，但 API 本身没有保证这种组合安全。

建议：

- 让 timer 绑定关系受 EventLoop 生命周期管理，在 `run()` 退出或 EventLoop 析构前主动 detach；
- 避免 LogContext 在 EventLoop 运行期之外长期持有裸指针；
- 短期调整应用对象析构顺序，在 EventLoop 析构前 shutdown/detach logging；
- 增加“局部 EventLoop 内记录 buffered 日志，EventLoop 先析构”的生命周期测试。

### 2.3 每条启用日志会清零约 17KB，并对每个 Appender 重复格式化

[`LogLine`](../src/log/LogLine.h#L23) 内含 8192 字节消息数组，当前通过 `{}` 对整块数组清零。ConsoleAppender 和 FileAppender 又分别创建并清零一个 9216 字节格式化数组。使用 Clang 22、`-O3` 检查生成代码时，`sizeof(LogLine)` 为 8264 字节，仍会生成一次 8192 字节 `memset`；每个目标 Appender 还会再生成一次 9216 字节 `memset`。因此单条启用日志的峰值栈占用至少约 17KB，并写入大量随后会被覆盖的零字节。

此外，[`Logger::dispatch()`](../src/log/Logger.cpp#L15) 将同一个 LogEvent 依次交给所有 Appender，每个 Appender 都重新执行完整格式化，包括 `localtime_r`、`strftime`、logger/source/message 拼接。同一条日志路由到多个目标时，这部分成本线性重复。

建议：

- 固定数组不做零初始化，只依靠有效长度控制读写；
- 在 Logger dispatch 层将最终日志行格式化一次；
- Appender 改为接收已格式化的连续字节视图；
- 考虑使用可重用的线程本地 scratch buffer，同时保留重入保护；
- 缓存秒级日期/时区前缀，只为每条日志追加微秒部分；
- 对 EventLoop 之外的系统线程 ID 使用线程本地缓存，避免每条日志执行 `gettid` syscall。

该项预计是当前收益最大、实现风险相对可控的性能重构。

### 2.4 flush timer 在空闲状态下仍会永久周期唤醒

处理状态：已按“一次性最早 deadline timer”方案完成整改。timer 现在只由实际非空 buffer 建立，所有
buffer 清空后停止调度，多个 buffer 直接调度到最早的 `flush_at`。

整改前，[`LoggerManager::current_context()`](../src/log/LoggerManager.cpp#L312) 只要发现配置中存在 buffered appender，就会在当前 EventLoop 上挂载 flush timer。timer 回调 [`on_context_timer()`](../src/log/LoggerManager.cpp#L337) 无论是否存在待写数据，都会按照全局最小 flush interval 重新 arm。

整改前的影响是：一个 EventLoop 只要产生过一次日志，即使所有 buffer 已经清空，也会永久周期唤醒并遍历全部 Appender。任何日志都会调用 `current_context()`，即使该 Logger 最终只路由到 console 或未缓冲文件，所以 timer 的建立范围也大于实际需要。

整改内容：

- 仅在某个 buffer 从空变为非空时 arm timer；
- flush 后若所有 buffer 都为空，则停止调度；
- 直接调度最早的 `flush_at`，而不是按全局最小 interval 轮询所有 Appender；
- 增加空闲 EventLoop 不持续唤醒的测试或计数型基准。

## 3. 中等优先级问题

### 3.1 `noexcept` 路径仍可能因为内存分配终止进程

文件滚动相关函数位于 `noexcept` 调用链上，但 [`rotate_locked()`](../src/log/Appender.cpp#L492) 会创建和扩展 `std::string`，retention cleanup 也会复制路径字符串。内存分配失败时异常无法离开 `noexcept` 函数，进程会调用 `std::terminate`。

初始化路径同时混用了 `new (std::nothrow)`、可返回 null 的 arena 和可能抛出 `std::bad_alloc` 的 `std::vector`/`std::string`，所以 `LogConfigErrorCode::OutOfMemory` 只能覆盖部分分配失败，错误处理语义并不完整。

建议预分配滚动需要的路径缓冲和 archive 元数据，或明确统一项目的 OOM 策略，不应让一部分 OOM 返回 `expected`、另一部分直接终止。

### 3.2 LoggerHandle 的生命周期约束没有由类型保证

[`LoggerHandle`](../src/log/Logger.h#L50) 公开接受任意 `std::string_view`，并在构造时把自身地址注册进全局侵入式链表。设计要求 Handle 具有静态存储期且 name 来自进程期字符串字面量，但当前 API 允许以下误用：

- 用临时 `std::string` 构造，导致 name 立即悬空；
- 创建局部 LoggerHandle，析构后注册表仍持有其地址；
- 在 registry seal 后构造对象，使后续初始化永久报告 late registration。

建议限制为字符串字面量构造，并让非静态 Handle 可以安全注销，或者把直接构造接口隐藏到 `DEFINE_LOGGER` 使用的内部工厂中。

### 3.3 文件路径去重只比较原始字符串

[`LogConfigBuilder::add_file_appender()`](../src/log/LogConfig.cpp#L134) 只检查 path 非空，并使用原始字符串相等判断重复。嵌入 NUL、`a.log` 与 `./a.log`、符号链接或硬链接都可能让多个 Appender 实际写入同一文件，但分别持有 mutex、fd、滚动序号和 retention 状态。

建议：

- 拒绝包含 NUL 的路径；
- 应用层先完成绝对路径解析和规范化；
- Appender 打开后使用 `(st_dev, st_ino)` 检测实际文件重复；
- 对允许指向设备或管道的非滚动 FileAppender 明确定义行为。

### 3.4 buffer 内存预算尚未落实

当前 LogContext 会按 buffered appender 数量一次性分配 `LogBuffer[]`，数据区则按 Appender 延迟分配。总体数据区开销约为：

```text
日志线程数 × buffered appender 数 × buffer_size
```

本设计前文要求配置阶段限制总 buffer 预算，但现有 `LogConfigBuilder` 没有相应上限或预算字段。Appender 数量较多或 buffer size 较大时，内存消耗会随工作线程数快速放大。

建议至少限制单个 Appender buffer size、单个 LogContext 的配置总预算，并由应用结合 worker 数量检查全局预算。

### 3.5 存在未使用状态和文档漂移

`LogEvent::function` 和 `LogLine::function_` 会在每条日志中传递，但默认 formatter 没有输出 function。应选择输出该字段或删除相关状态。

现有 [`log_system.md`](log_system.md) 的部分 V1 描述已落后于代码，例如仍将进程内文件滚动列为非目标、仍描述文件短写不补写，以及部分接口草图未包含 stats 和内部滚动能力。后续修改实现时应同步更新设计文档，而不是长期让设计约束与实际行为并存。

## 4. 生命周期契约说明

当前 LoggerHandle 指针和 LoggerManager runtime 都不是原子对象，线程本地 buffer 也不会被 LoggerManager 跨线程遍历。因此当前实现仍要求：

1. 在启动业务线程前完成 LoggerManager 初始化；
2. shutdown 前停止并 join 所有日志生产线程和 EventLoop；
3. `initialize()`、`shutdown()`、`reopen_all()` 不与日志写入或彼此并发；
4. 普通线程若长期存活且使用 buffered appender，需要显式 flush 或在 LoggerManager shutdown 前退出；
5. 与 LogContext 绑定过的 EventLoop 必须活到 context detach 完成。

在遵守上述契约时，Logger 和 runtime 的普通读取路径不需要原子操作。建议将这些要求从设计文档同步到公开头文件注释，并在 Debug 构建中尽可能通过状态和线程断言验证。

## 5. 推荐实施顺序

1. 修复 LogContext/EventLoop 生命周期。
2. 去除两个大数组的无意义清零，并将格式化收敛为每个 LogEvent 一次。
3. 将 flush timer 改为只在存在待写数据时调度。
4. 统一 OOM/noexcept 策略，收紧 LoggerHandle 和文件路径契约。
5. 增加局部 EventLoop 析构、buffered reopen、并发 reopen 和滚动失败路径测试。
6. 完成上述正确性与同步性能优化后，再通过基准决定是否需要异步 writer；异步 writer 会引入队列容量、背压、丢弃策略和退出 drain 等额外语义，不应在缺少基准数据时优先引入。

## 6. 建议新增的验证项

- 通过可控 write 故障注入覆盖 ConsoleAppender 的 `EINTR`、短写后失败和零进展路径；
- buffered LogContext 与局部 EventLoop 的两种析构顺序；
- EventLoop buffer 清空后不再周期唤醒；
- 同一 LogEvent 路由多个 Appender 时只格式化一次；
- LoggerHandle 临时字符串和非静态生命周期误用被编译期或运行期拒绝；
- 等价路径、嵌入 NUL、硬链接和符号链接的重复文件检测；
- reopen 与并发 direct/buffered write；
- 滚动路径分配失败、archive 创建失败、retention 删除失败；
- 初始化阶段各类分配失败遵循统一的错误处理策略；
- 启用/禁用日志的栈占用、CPU 周期、分配次数和多 Appender 扩展曲线。
