# 日志系统重构复核

## 结论

日志实现已经从“生产线程同步格式化和写入、每线程持有 FileAppender buffer”重构为“生产线程构造 owning record、单日志线程统一格式化和写入”。

本次重构解决了原实现的三个结构性问题：

- 固定 8192 字节消息区和固定格式化区造成的单条日志截断；
- `LogContext` 持有生产线程 EventLoop 指针带来的 timer 生命周期耦合；
- 多个生产线程同时写 Appender，需要文件锁且每条多目标日志重复格式化。

## 已落地的不变量

### 单一写入所有者

一个 `LoggerManager` 只有一个 `LogWorker`。所有 FileAppender、stdout 和 stderr 的正常配置写入都发生在该线程。Appender 的 fd、buffer、轮转和 retention 状态不再加锁。

`AppenderStats::writer_thread_id` 与 `LogQueueStats::writer_thread_id` 可用于验证实际写线程。

### 无固定单条大小上限

`LogLine` 不再在栈上保存 8192 字节数组。`OwnedLogRecord` 使用 inline 区域和 growable chunk 链保存完整消息。大记录通过分段 `writev` 写入，不要求构造等大的连续格式化副本。

backlog 容量不是单条大小限制。大于容量的记录在队列为空时获得独占准入。

### 不重复增加 MPSC

每条 `OwnedLogRecord` 内嵌一个 `EventLoop::NotifyEntry`，直接提交给日志 EventLoop。日志层没有额外的 MPSC 队列。

EventLoop 内部负责：

- 多生产者入队；
- eventfd 唤醒；
- 唤醒合并；
- 回调前重置队列节点。

最后一点允许记录回调在处理结束时直接释放自身。

### 生产线程不批量缓存完整记录

生产线程只构造当前一条记录，完成后立即投递。文件写合并集中在日志线程内的 FileAppender buffer。这样不需要生产线程 timer、线程退出 flush 或跨 EventLoop detach。

### 控制面与数据面统一排序

`Flush`、`Reopen` 和 `StopAfterDrain` 也通过 `NotifyEntry` 投递。控制操作与此前已入队记录共享顺序。

停止命令先转换为日志线程本地 deferred callback。本轮 notify 排空后才执行最终 flush 和 `EventLoop::stop()`，避免停止回调早于同批记录完成。

## 背压复核

`LogBacklog` 按记录实际分配字节数记账，默认 64 MiB 和 `Block` 策略。容量状态使用：

- 普通记录占用字节；
- 超大记录独占哨兵；
- 独立的停止接收标志和唤醒序号。

超大记录的实际大小不写入容量状态，因此没有被状态位宽限制。容量释放或 shutdown 会推进唤醒序号并唤醒阻塞生产者。控制消息不受 backlog 容量限制。

仍需遵守生命周期前置条件：业务生产线程必须先停止并 join，再调用 `LoggerManager::shutdown()`。shutdown 不负责使已经缓存的 runtime `Logger*` 跨销毁并发安全。

## 文件写入与轮转复核

FileAppender buffer 现在每个 Appender 只有一份，由日志线程拥有。buffer deadline 由日志线程上的单个最早 timer 管理；持续 notify 流量下也会在每条记录后主动检查到期时间。

轮转按完整记录边界执行：

- 加入记录将超过阈值时，在写该记录前轮转；
- 超过阈值的单条记录允许完整写入空文件；
- 不拆分或截断日志；
- retention 也在日志线程串行执行。

`reopen_all()` 先 flush 再替换 fd，队列中位于 reopen 前后的记录会写到对应的旧文件和新文件。

## 保留的同步异常路径

以下路径不会投递到日志线程：

- LoggerManager 初始化前和 shutdown 后的 bootstrap 日志；
- allocation、write、reopen、rotation、retention 等内部故障诊断。

这些路径直接、限频地写 stderr，避免日志系统故障再次进入自身队列。它们不属于“所有已配置 Appender 由一个线程写”的正常路径承诺。

## 验证范围

`LogSystemTest` 覆盖：

- 2 MiB 单条消息在 64 KiB backlog 下独占准入且完整落盘；
- `DropNewest` 和 shutdown 对阻塞准入的唤醒；
- 流式长日志不截断；
- 同一格式化记录分发给 direct 和 buffered Appender；
- 多个 Appender 的实际 writer thread ID 一致；
- 低流量 buffer 由专用日志 timer flush；
- 生产线程退出不再承担 buffer flush；
- reopen 与记录的顺序；
- 超过轮转阈值的完整记录与 archive retention；
- 多生产者并发时每条文件和 console 记录保持完整；
- shutdown 排空已投递记录；
- raw append 保持字节不变，编码失败时可取消半条记录；
- 安全文件模式拒绝符号链接、强制权限并恢复不完整尾行。

实现无法承诺进程崩溃、内存耗尽、文件系统永久阻塞或 `SIGKILL` 下无日志损失；这些属于持久化与故障模型的后续议题。
