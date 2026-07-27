# 日志系统设计

## 1. 目标与核心约束

日志系统采用单独的日志线程完成所有已配置 Appender 的写入。业务线程只负责判断日志是否启用、构造一条拥有自身数据的记录并投递，不执行格式化、文件写入、buffer flush、reopen 或轮转。

核心约束如下：

- 单条日志没有配置层面的大小上限，不再截断为固定长度；
- 同一 `LoggerManager` 中的所有文件 Appender 和 console Appender 都由同一个日志线程写；
- 一条完整记录直接使用自身内嵌的 `EventLoop::NotifyEntry` 投递，不再增加一层日志 MPSC 队列；
- 生产线程不聚合多条日志，记录完成后立即投递；
- 一条记录在日志线程格式化一次，再分发到全部目标 Appender；
- 文件 buffer 只属于日志线程，不再存在生产线程本地 `LogContext`；
- 已准入日志队列的内存按容量受控，默认使用阻塞背压；
- 初始化前、关闭后和内部故障诊断允许同步写原始 stderr，它们不属于正常异步写入路径。

## 2. 总体结构

```text
业务线程 A ─┐
业务线程 B ─┼─ OwnedLogRecord + NotifyEntry ─> 单日志 EventLoop
业务线程 C ─┘                                  │
                                                ├─ format once
                                                ├─ ConsoleAppender
                                                ├─ FileAppender A + buffer
                                                └─ FileAppender B + buffer
```

`EventLoop::post()` 已经包含线程安全的 notify 队列与 eventfd 唤醒合并，因此日志层不再维护第二个 MPSC 队列。每条记录同时是队列节点和消息所有者，日志线程处理完成后可以直接释放记录。

一个 `LoggerManager` 只创建一个 `LogWorker` 和一个 `EventLoopGroup(1)`。即使配置多个日志文件，也不会为每个文件创建线程。这样可以获得：

- 所有 Appender 状态只有一个写线程；
- 不需要 Appender 写锁；
- `flush`、`reopen`、轮转和记录写入拥有统一顺序；
- 同一条记录的多目标分发天然串行；
- 线程数量不随日志文件数量增长。

如果以后证明单线程写盘是实际瓶颈，应新增明确的分片模型，而不是让每个 Appender 隐式创建线程。分片后跨文件顺序、控制屏障和 shutdown 都需要重新定义。

## 3. 生产线程路径

### 3.1 Logger 路由

初始化时，`LoggerManager` 展开 logger 层级、level、additive 和 Appender level 范围。每个 level 最终保存一段连续的 `AppenderId` 数组。

日志宏先读取对应 level 的目标数组：

- 数组为空时不构造 `LogLine`，表达式也不求值；
- 数组非空时构造 `LogLine`；
- 目标使用 `AppenderId`，生产线程不访问 Appender 对象。

Logger、logger 名称和目标数组都由 runtime arena 持有，在 shutdown 排空日志队列前保持稳定。

### 3.2 OwnedLogRecord

`OwnedLogRecord` 持有一条异步记录所需的全部可变数据：

- logger 名称；
- level、时间戳、生产线程标识；
- source file、line 和 function；
- 目标 `AppenderId` 数组；
- 消息数据；
- 内嵌 `EventLoop::NotifyEntry`；
- 背压记账状态。

消息先写入记录内的 inline 区域，超出后使用分段、递增容量的 chunk 链。没有 `8192`、`64 KiB` 或其他业务大小上限，也不写入 `<truncated>`。实际可写大小仍受以下物理条件约束：

- `size_t` 可表示范围；
- 进程可用内存；
- backlog 准入策略；
- 最终文件系统和设备能力。

分段存储避免为了完成一条大日志反复复制整个历史消息。构造失败时整条记录丢弃并增加 allocation failure 统计，不提交半条记录。

### 3.3 不在生产线程聚合多条记录

生产线程只在当前 `LogLine` 内积累一条记录。`LogLine` 析构后立即调用 `EventLoop::post()`。

不在生产线程累积多条完整记录，原因是：

- 会增加低流量日志的可见延迟；
- 每个生产线程都需要 timer、buffer 和退出 flush；
- 重新引入线程本地生命周期问题；
- EventLoop 的 eventfd 唤醒已经做合并，多个相邻 post 不等于多个系统调用；
- 文件写合并应在唯一写线程的 Appender buffer 中完成。

## 4. 投递与背压

### 4.1 直接 NotifyEntry

成功完成的记录按以下顺序提交：

1. 计算记录实际分配字节数；
2. 向 `LogBacklog` 申请容量；
3. 将 `worker` 写入记录；
4. 直接调用日志 EventLoop 的 `post<OwnedLogRecord, ...>()`；
5. 日志线程回调格式化、分发、释放记录并归还容量。

控制命令也使用自身内嵌的 `NotifyEntry`。记录和控制命令因此共享同一条 EventLoop 顺序通道。

### 4.2 容量模型

默认 backlog 容量是 64 MiB，按记录的实际分配字节数记账，而不是只按消息长度记账。统计包括：

- 当前排队记录数和字节数；
- 峰值排队记录数和字节数；
- queue-full 丢弃数；
- allocation failure；
- formatting failure；
- 日志线程 ID。

记录必须先构造完整才能知道最终大小，因此 backlog 容量约束的是已经准入的记录，不是进程 RSS 的硬上限。容量等待期间，每个被阻塞的生产线程仍持有自己当前的一条完整记录；正在构造的记录也尚未记入 backlog。系统不会在生产线程保存第二条或一批待投递记录。

支持两种 full policy：

- `Block`：默认策略。生产线程等待容量释放，保证正常运行期不因短时突发丢日志；
- `DropNewest`：容量不足时丢弃当前新记录，适合不能接受生产线程阻塞的场景。

### 4.3 超大记录

单条记录可能大于整个 backlog 容量。为了同时满足“单条日志不限制大小”和“排队内存有界”，使用独占准入：

- backlog 为空时，允许一条超容量记录独占进入；
- 在该记录处理完成前，不再准入其他记录；
- `Block` 等待独占记录完成；
- `DropNewest` 在无法立即获得独占准入时丢弃新记录。

因此 backlog 容量限制并发在途记录的总量，不是单条记录上限。

## 5. 日志线程

### 5.1 记录处理

日志线程收到记录后：

1. 使用线程持有的 `LogFormatter` 生成 prefix；
2. 将 prefix、消息分段和末尾换行组织为 `FormattedLogRecord`；
3. 依次调用目标 Appender；
4. 检查到期的文件 buffer；
5. 更新最早 flush timer；
6. 释放记录并归还 backlog 容量。

`FormattedLogRecord` 是非 owning 的分段视图，只在当前回调内使用。多个 Appender 共享同一份格式化结果。大记录写入使用固定大小 `iovec` 数组分批 `writev`，不需要再分配一块与整条日志等大的连续格式化 buffer。

### 5.2 时间与 timer

日志线程持有一个 flush timer，始终指向所有 FileAppender 中最早的非空 buffer deadline。

日志 notify 可能在持续高流量下长时间排空。为避免 timer 因 notify 饥饿而延后，每次记录处理都会用新的 `steady_clock::now()` 检查到期 buffer；低流量或空闲时则由 EventLoop timer 触发。

### 5.3 控制命令

以下操作通过日志 EventLoop 投递控制命令并等待完成：

- `Flush`：排在命令前的记录处理完后 flush 全部 Appender；
- `Reopen`：先 flush，再在日志线程替换所有文件描述符；
- `StopAfterDrain`：等待本轮 notify 排空，flush 全部 buffer，取消 timer，然后停止 EventLoop。

控制命令不占用 backlog 容量，保证队列满时仍能 flush 和退出。

对于同一生产线程，post 顺序就是处理顺序。多个生产线程之间的顺序由 notify 队列的实际入队顺序决定，而不是由记录时间戳重新排序。

## 6. Appender

### 6.1 ConsoleAppender

stdout/stderr 写入只发生在日志线程。记录可能由多次 `writev` 完成，但不会与其他正常异步日志记录并发写。

初始化前、shutdown 后的 bootstrap 日志和内部 I/O 故障提示仍使用同步 stderr；这些是有意保留的异常路径。

### 6.2 FileAppender

FileAppender 的以下状态全部只在日志线程访问：

- 文件描述符；
- 可选连续写 buffer；
- buffer 中的记录数、字节数和 flush deadline；
- 当前文件大小；
- rotation retry；
- archive 列表与 retention retry。

未配置 buffer 时，记录直接按分段写入。配置 buffer 时：

- 能放入 buffer 的完整记录复制到连续 buffer；
- 放不下时先 flush；
- 单条记录大于 buffer 时先 flush 已有内容，再直接分段写入；
- buffer 满、deadline 到期、显式 flush、reopen 和 shutdown 都会 flush。

buffer 的用途是合并文件系统写入，不是延迟生产线程投递。

### 6.3 文件轮转

轮转在写入每条记录前判断，按“已写字节 + 当前 buffer + 新记录”计算：

- 当前文件非空且加入新记录会超过阈值时，先 flush 旧 buffer，再轮转；
- 单条记录本身大于轮转阈值时，允许它写入空的新文件；
- 下一条记录会触发后续轮转；
- 不拆分一条日志来满足轮转大小。

因此 `max_file_size` 是轮转触发阈值，不再要求大于某个“最大日志行”常量。

## 7. 生命周期

### 7.1 初始化

`LoggerManager::initialize()` 的顺序是：

1. 校验配置；
2. 创建并打开所有 Appender；
3. 编译 logger 路由；
4. 创建日志线程；
5. 通过 `Ready` 控制消息确认 EventLoop 已运行；
6. 发布 runtime；
7. 将所有静态 `LoggerHandle` 绑定到 runtime Logger。

任一步失败都不会把 Handle 留在半初始化 runtime 上。

### 7.2 关闭

调用方必须先停止并 join 所有可能产生日志的业务线程，然后调用 `shutdown()`。关闭顺序是：

1. 将 `LoggerHandle` 重新绑定到 bootstrap Logger；
2. backlog 停止接收新记录并唤醒等待者；
3. 投递 `StopAfterDrain`；
4. 日志线程排空已投递记录；
5. flush 全部 Appender；
6. 停止并 join 日志线程；
7. 销毁 Appender、Logger 和 arena。

shutdown 不与仍在运行且缓存了 runtime `Logger*` 的生产者并发。这个前置条件保证 logger 名称、目标数组和 runtime 在所有记录处理完成前保持有效。

## 8. API 语义

- `LoggerManager::flush()`：全局异步队列屏障，并 flush 所有 Appender；
- `flush_current_thread()`：为兼容旧调用保留，语义已经等同于全局 `flush()`；
- `reopen_all()`：在日志线程中按队列顺序执行 flush 和 reopen；
- `appender_stats(id)`：读取 Appender 原子统计；
- `queue_stats()`：读取 backlog 与日志线程统计；
- `shutdown()`：停止接收、排空、flush 并 join。

`log_complete_message()` 与流式 `LogLine` 使用同一个 `OwnedLogRecord` 提交路径。

## 9. 故障策略

- 记录或 chunk 分配失败：丢弃整条记录，增加 allocation failure，并限频输出原始 stderr；
- backlog 满：按 `Block` 或 `DropNewest` 执行；
- 格式化分配失败：丢弃整条记录，增加 formatting failure；
- 文件或 console 部分写失败：记录已写字节，增加 write error 和 dropped record；
- reopen、rotation、retention 失败：保留对应统计并限频输出原始 stderr；
- 内部诊断不重新进入日志系统，避免递归和死锁。

该设计保证记录不会因配置的固定行长被截断，但不能在内存耗尽、永久 I/O 阻塞或进程异常终止时承诺无损。
