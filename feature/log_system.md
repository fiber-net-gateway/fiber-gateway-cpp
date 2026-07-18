# C++ 高性能日志系统设计方案

## 1. 设计定位

日志系统组合三类成熟模型：

- 调用方式接近 glog：使用 `LOG`、`LOG_IF`、`DLOG`、`VLOG` 和流式 `<<`。
- 文件写入接近 nginx：文件以 `O_APPEND` 打开，日志在调用线程同步执行一次 `write()`。
- 路由方式接近 logback：Logger 使用层级 name，具名 Appender 根据 Logger name、level 和 additivity 决定输出文件。

本设计不引入异步日志线程。同步模式下，产生日志的线程负责格式化并写文件；可选 buffer 模式也由当前线程在 buffer 满或定时器触发时执行 flush。

### 1.1 目标

- Logger name 与输出文件解耦。
- 多个 Logger 可以共享一个 Appender 和文件描述符。
- 支持 Logger 层级、级别继承和 Appender additivity。
- 支持多线程共享 `O_APPEND` 文件描述符。
- 禁用日志时不构造日志对象、不求值 `<<` 后的表达式。
- 常见日志在业务线程中不发生动态内存分配。
- 可选每线程/每 EventLoop buffer，减少 `write()` 次数。
- 配置和文件打开在启动阶段完成，日志热路径不解析配置、不匹配字符串规则。

### 1.2 非目标

V1 不处理以下能力：

- 不处理一次 `write()` 未写完的情况；一次调用的返回值小于请求长度时，该条或该批日志视为写入失败。
- 不保证日志已经持久化到磁盘；默认不调用 `fsync()`。
- 不提供专用异步 writer 线程和异步队列。
- 不提供进程内按大小或时间滚动文件；文件滚动交给外部工具，日志系统只提供 reopen。
- 不保证不同线程日志严格按照 timestamp 排序。
- 不支持运行时热更新 Logger 路由；V1 在启动时创建不可变 Logger。

---

## 2. 核心概念

```text
DEFINE_LOGGER
  │
  ▼
LoggerHandle(name)
  │
  │ initialize 后绑定
  ▼
Logger
  │
  ├── LevelTargets[TRACE] -> first + count
  ├── LevelTargets[DEBUG] -> first + count
  ├── LevelTargets[INFO]  -> first + count
  ├── LevelTargets[WARN]  -> first + count
  ├── LevelTargets[ERROR] -> first + count
  └── LevelTargets[FATAL] -> first + count
                              │
                              ▼
                           Appender
                              │
                              ├── ConsoleAppender
                              └── FileAppender
                                     ├── direct: format -> write(O_APPEND)
                                     └── buffer: format -> local buffer -> write(O_APPEND)
```

### 2.1 LoggerHandle

文件级 `DEFINE_LOGGER` 在 `main()` 之前执行，但 Appender 需要在应用读取配置后才能创建。因此 `DEFINE_LOGGER` 创建的是静态 `LoggerHandle`，而不是最终 Logger。

LoggerHandle 保存：

- 生命周期覆盖整个进程的 Logger name；
- 指向最终 Logger 的非 owning 指针；
- 用于启动阶段枚举所有 LoggerHandle 的侵入式注册节点。

初始化前，Handle 指向 bootstrap Logger；初始化成功后，一次性绑定到最终 Logger。V1 要求绑定发生在业务线程启动之前，因此热路径不需要原子操作。

LoggerHandle 注册表头必须使用安全的常量初始化，Handle 构造只执行无分配的侵入式链表插入，不依赖 LoggerManager 的动态初始化顺序。

### 2.2 Logger

最终 Logger 在所有 Appender 创建成功后由 LoggerManager 动态创建。Logger 保存：

- 层级 name，例如 `gateway.auth`、`gateway.http.access`；
- verbosity；
- 每个 LogLevel 对应的 `LevelTargets`。

每个 `LevelTargets` 只保存：

- 该 level 的第一个 Appender 指针；
- Appender 数量。

Logger 不保存文件路径、文件描述符、buffer、flush 或 reopen 状态，也不拥有 Appender。

### 2.3 Appender

Appender 是具名输出目标，例如 `auth_file`、`gateway_file`、`all_file`。Appender 负责：

- 打开并持有一个输出文件描述符；
- 格式化最终日志行；
- 同步写入或当前线程 buffer；
- flush 和 reopen；
- 记录写入错误和丢弃数量。

同一个 Appender 只创建一个实例。多个 Logger 引用相同 Appender 时，共享其 fd 和文件状态。

### 2.4 Logger 变长布局

Logger 创建前，LoggerManager 先根据 Logger 层级、additivity 和 Appender filter，计算每个 level 的目标列表及长度。随后只执行一次变长分配：

```text
┌────────────────────────────────────┐
│ Logger                             │
│                                    │
│ name                               │
│ verbosity                          │
│ levels[TRACE] -> first, count      │
│ levels[DEBUG] -> first, count      │
│ levels[INFO]  -> first, count      │
│ levels[WARN]  -> first, count      │
│ levels[ERROR] -> first, count      │
│ levels[FATAL] -> first, count      │
├────────────────────────────────────┤
│ 连续 Appender* 指针大数组          │
│                                    │
│ INFO:  auth, gateway, all          │
│ WARN:  auth, gateway, all          │
│ ERROR: auth, gateway, all, stderr  │
│ FATAL: auth, gateway, all, stderr  │
└────────────────────────────────────┘
```

Logger 本体和所有 level 的 Appender 指针位于同一块 Logger arena 内存中。各 Level Slot 指向尾随指针区的相应区间；空 level 使用 `first = nullptr, count = 0`。

多个源文件可以声明相同 name 的 LoggerHandle。LoggerManager 按 name 分组，每个不同 name 只创建一个最终 Logger，并把所有同名 Handle 绑定到该对象。

启动阶段可以使用临时 `std::vector<Appender*>` 解析规则。Logger 创建完成后释放临时容器，运行时不保留 vector，也没有固定 Appender 数量上限。

热路径只需要：

1. 读取 level 对应的 `LevelTargets`；
2. count 为 0 时直接关闭该条日志；
3. 构造消息并遍历 `[first, first + count)`。

热路径不执行 Logger name 匹配、level 继承、additivity、Appender filter 或动态分配。

---

## 3. 使用接口

### 3.1 定义 Logger

每个源文件可以定义一个或多个 Logger：

```cpp
DEFINE_LOGGER(AUTH, "gateway.auth");
DEFINE_LOGGER(HTTP_ACCESS, "gateway.http.access");
DEFINE_LOGGER(QUIC_PACKET, "gateway.quic.packet");
```

宏生成一个具有静态存储期的 `LoggerHandle`。Logger name 必须是生命周期覆盖整个进程的字符串字面量。

概念上等价于：

```cpp
static fiber::log::LoggerHandle AUTH{"gateway.auth"};
```

LoggerHandle 构造时只登记 name，不解析规则、不创建 Appender，也不分配最终 Logger。LoggerManager 初始化成功后再绑定实际 Logger。

V1 要求 `DEFINE_LOGGER` 只能用于 namespace/file scope，并且所有 LoggerHandle 必须在 LoggerManager 初始化前完成注册。初始化完成后不再接受新的 LoggerHandle。

### 3.2 普通日志

```cpp
LOG(AUTH, INFO)
    << "user login id="
    << id;
```

LoggerHandle 在宏内部只读取一次。概念上等价于：

```cpp
const Logger& logger = AUTH.get();
if (logger.enabled(LogLevel::Info)) {
    LogLine(logger, LogLevel::Info, __FILE__, __LINE__, __func__)
        << "user login id="
        << id;
}
```

`LogLine` 析构时完成该条消息，并同步分发到 Logger 对应 level 的 Appender 数组。

### 3.3 条件日志

```cpp
LOG_IF(AUTH, WARN, retry > 3)
    << "retry too many";
```

条件只求值一次。条件为 false 或 level 被禁用时，不构造 `LogLine`，也不求值消息表达式。

### 3.4 Debug 日志

```cpp
DLOG(AUTH, INFO)
    << "debug message";
```

Release 构建可以在编译期完全移除 `DLOG`。被移除时，消息表达式不得求值。

### 3.5 Verbose 日志

```cpp
VLOG(QUIC_PACKET, 2)
    << "packet number="
    << packet_number;
```

每个 Logger 具有继承得到的 verbosity。只有 `configured_verbosity >= requested_verbosity` 时才生成日志。VLOG 默认使用 Debug 级别输出。

### 3.6 宏安全要求

- 可以安全出现在 `if/else` 中，不产生悬空 `else`。
- Logger、条件以及消息表达式都不会被重复求值。
- 禁用路径不分配内存、不格式化消息、不获取 wall clock。
- `LOG`、`LOG_IF`、`DLOG`、`VLOG` 的内部实现不得抛出 C++ 异常。

---

## 4. Logger name 与 logback 式层级

Logger name 使用 `.` 分隔层级：

```text
gateway
gateway.auth
gateway.http
gateway.http.access
gateway.quic.packet
```

配置项 `gateway.http` 同时作用于：

```text
gateway.http
gateway.http.access
gateway.http.client
```

但不匹配：

```text
gateway.http2
gateway.http_client
```

匹配必须位于完整的 name segment 边界。

### 4.1 最接近规则

Logger 首先查找 name 最长、最具体的配置项。如果没有完全同名配置，则依次查找父级：

```text
gateway.http.access.detail
gateway.http.access
gateway.http
gateway
root
```

### 4.2 Level 继承

Logger 的有效 level 来自最近一个显式配置了 level 的父级规则；如果都没有，则使用 root level。

例如：

```text
root:                 INFO
gateway:              DEBUG
gateway.quic.packet:  TRACE
```

得到：

```text
other                  -> INFO
gateway.auth           -> DEBUG
gateway.http.access    -> DEBUG
gateway.quic.packet    -> TRACE
```

### 4.3 Appender additivity

Logger 先写入最近规则绑定的 Appender。如果该规则 `additive = true`，继续向父级 Logger 传播，最终可以传播到 root。

例如：

```text
gateway.auth -> auth_file
gateway      -> gateway_file
root         -> all_file
```

当 `gateway.auth` 和 `gateway` 都启用 additivity 时：

```text
LOG(gateway.auth, INFO)
    -> auth_file
    -> gateway_file
    -> all_file
```

当 `gateway.auth.additive = false` 时：

```text
LOG(gateway.auth, INFO)
    -> auth_file
```

如果同一个 Appender 通过多个父级重复出现，LoggerManager 在创建 Logger 前对该 level 的目标列表去重，避免同一条日志重复写入同一个目标。

---

## 5. 配置模型

核心日志库只接收类型化 `LogConfig`，不负责解析 YAML、XML 或应用配置文件。具体应用负责把自己的配置语法转换成 `LogConfig`。

对于 `apps/lite_nginx`，建议沿用现有 nginx 风格语法：

```nginx
logging {
    appender all_file {
        type file;
        path logs/all.log;
        mode 0644;
        buffer_size 32k;
        flush_interval 1s;
    }

    appender gateway_file {
        type file;
        path logs/gateway.log;
        mode 0644;
    }

    appender auth_file {
        type file;
        path logs/auth.log;
        mode 0640;
    }

    appender stderr {
        type console;
    }

    logger gateway {
        level debug;
        appender gateway_file;
        additive on;
    }

    logger gateway.auth {
        level info;
        appender auth_file;
        additive on;
    }

    logger gateway.quic.packet {
        level trace;
        verbosity 2;
        additive on;
    }

    root_logger {
        level info;
        appender all_file;
        appender stderr level=error;
    }
}
```

该配置产生以下路由：

```text
gateway.auth          -> auth_file, gateway_file, all_file
gateway.http          -> gateway_file, all_file
gateway.quic.packet   -> gateway_file, all_file
other                 -> all_file
ERROR 及以上           -> 在上述结果之外写入 stderr
```

配置中的路径解析规则应与 lite_nginx 现有文件路径规则一致：相对路径相对于包含该配置项的配置文件目录解析，而不是相对于进程工作目录解析。

### 5.1 C++ 启动 API

应用先通过 `LogConfigBuilder` 注册所有具名 Appender，再添加 Logger 规则并设置 root，最后一次性初始化 `LoggerManager`：

```cpp
fiber::log::LogConfigBuilder builder;

auto all_file = builder.add_file_appender({
    .name = "all_file",
    .path = resolved_log_dir + "/all.log",
    .file_mode = 0644,
    .buffer_size = 32 * 1024,
    .flush_interval = std::chrono::seconds(1),
});
if (!all_file) {
    return startup_error(all_file.error());
}

auto error_console = builder.add_console_appender({
    .name = "stderr",
    .stream = fiber::log::ConsoleStream::Stderr,
    .min_level = fiber::log::LogLevel::Error,
});
if (!error_console) {
    return startup_error(error_console.error());
}

auto gateway = builder.add_logger(
    {
        .name = "gateway",
        .level = fiber::log::LogLevel::Debug,
        .additive = true,
    },
    {*all_file});
if (!gateway) {
    return startup_error(gateway.error());
}

auto root = builder.set_root_logger(
    {.level = fiber::log::LogLevel::Info},
    {*all_file, *error_console});
if (!root) {
    return startup_error(root.error());
}

auto config = builder.finish();
if (!config) {
    return startup_error(config.error());
}

auto initialized = fiber::log::LoggerManager::global().initialize(
    std::move(*config));
if (!initialized) {
    return startup_error(initialized.error());
}
```

`AppenderId` 只在启动配置阶段引用 Appender。`initialize()` 才会创建并打开 Appender、计算各 Logger 的 level 长度、分配变长 Logger 并提交所有 Handle 绑定。业务线程和 EventLoop 必须在 `initialize()` 成功后启动。

退出时先停止并 join 所有日志生产线程，再调用：

```cpp
fiber::log::LoggerManager::global().shutdown();
```

管理路径还提供 `flush_current_thread()`、`reopen_all()` 和 `appender_stats(id)`。这些接口不应与 `initialize()` 或 `shutdown()` 并发执行。

### 5.2 Appender filter

Appender 可以设置最小和最大 level，用于把日志按级别拆分到不同目标：

```nginx
appender error_file {
    type file;
    path logs/error.log;
    min_level error;
}
```

Appender filter 在创建 Logger 前完全展开到各 level 的目标列表。Appender 不接受某个 level 时，不把它写入该 Level Slot，因此热路径不再执行 Appender filter 判断。

### 5.3 配置校验

启动阶段必须拒绝以下配置：

- 重复的 Appender name；
- Logger 引用了不存在的 Appender；
- 重复且冲突的 Logger 配置；
- 非法 level、verbosity、buffer size 或 flush interval；
- 多个不同 Appender 指向同一路径但配置不一致；
- 单个 level 的 Appender count 或 Logger 总分配大小发生整数溢出；
- Logger arena 分配失败；
- 文件无法创建或打开。

配置和文件打开失败应阻止应用进入服务状态，而不是在收到请求后才发现日志不可写。

---

## 6. LogEvent 与消息构造

`LogEvent` 只在同步调用链中使用，因此可以使用非 owning view：

```cpp
struct LogEvent {
    std::string_view logger_name;
    std::string_view message;
    std::string_view file;
    std::string_view function;

    LogLevel level;
    std::uint32_t line;
    std::uint64_t timestamp_us;
    std::uint32_t thread_id;
};
```

约束：

- `logger_name` 指向静态 Logger name。
- `file` 和 `function` 指向编译器提供的静态字符串。
- `message` 指向 `LogLine` 的固定容量 buffer，只在本次同步 dispatch 期间有效。
- 如果 Appender 开启 buffer，必须在 dispatch 返回前把最终日志行复制到当前线程自己的 Appender buffer。

### 6.1 固定容量 LogLine

常见日志使用栈上或可重用的固定容量 buffer，并使用轻量转换：

- 整数和浮点数使用 `std::to_chars`；
- `string_view` 和字符串字面量直接复制；
- 不使用 `std::ostringstream`；
- 不在普通日志热路径构造 `std::string` 或 `std::vector`。

必须配置最大单条日志长度，例如 8KB。超过限制时截断，并在结尾追加明确标记：

```text
... <truncated>
```

最终日志行始终以一个 `\n` 结束。消息内部的 `\r`、`\n` 和其他控制字符应转义，保证一条日志对应一行文本。

---

## 7. 日志格式

默认格式：

```text
2026-07-18T15:04:05.123456+08:00 INFO  [worker=2] gateway.auth File.cpp:123 user login id=42
```

至少包含：

- wall clock timestamp 和微秒精度；
- level；
- thread 或 EventLoop index；
- Logger name；
- source file 和 line；
- message。

Formatter 在配置初始化时编译格式规则。热路径不得重复解析 pattern。

在 EventLoop 请求处理路径中，steady time 优先使用：

```cpp
fiber::event::EventLoop::current().now();
```

每个日志线程或 EventLoop context 可以缓存 steady clock 与 wall clock 的对应关系，并周期更新日期和秒级前缀，避免每条日志调用昂贵的时间格式化函数。

---

## 8. FileAppender 与 O_APPEND

### 8.1 文件打开

```cpp
int fd = ::open(path,
                O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
                file_mode);
```

约束：

- `O_APPEND` 必须启用；
- `O_CREAT` 必须传入明确的 mode；
- 使用 `O_CLOEXEC`，避免 fd 泄漏到子进程；
- 文件在 LoggerManager 初始化时打开；
- 所有引用同一 Appender 的 Logger 共享一个 fd。

### 8.2 同步直写模式

```text
LOG()
  │
  ├── level check
  ├── format one complete line
  └── write(fd, line, line_size)
```

每个 Appender 对每条日志只调用一次 `write()`：

```cpp
ssize_t written = ::write(fd, line, line_size);
```

本设计基于以下工程假设：

- Linux `O_APPEND` 保证定位文件末尾和该次 write 作为一个原子追加操作；
- 一次日志已经在用户态格式化成连续内存；
- 不处理成功但只写入部分数据的情况；
- 多线程之间不使用文件写锁。

`write()` 返回负值或返回长度不等于请求长度时，记录一次 Appender 写入错误并丢弃未完成部分，不进行补写。

### 8.3 顺序语义

- 同一线程的同步日志保持调用顺序。
- 多线程日志顺序由进入内核的 write 顺序决定。
- buffer 模式按照各线程 flush 的顺序追加，可能与日志 timestamp 顺序不同。
- 所有模式都不提供跨线程全局顺序保证。

---

## 9. 可选 Buffer 模式

Buffer 模式仍然是同步日志，只减少 syscall，不把文件 I/O 转移到其他线程。

```text
业务线程 / EventLoop
        │
        ▼
LogContext
        │
        ├── all_file buffer
        ├── gateway_file buffer
        └── auth_file buffer
                 │
                 ├── buffer full
                 ├── local timer
                 └── thread exit / shutdown
                         │
                         ▼
                   write(O_APPEND)
```

### 9.1 Buffer 所有权

Buffer 不直接放在共享 Appender 对象中。每个产生日志的线程或 EventLoop 持有一个 `LogContext`，其中为每个开启 buffer 的 Appender 保存一个独立 buffer。

因此内存开销近似为：

```text
日志线程数 × buffered appender 数 × buffer_size
```

配置阶段应计算并限制总 buffer 预算。

### 9.2 写入策略

追加一条日志前：

1. 如果剩余空间足够，复制完整日志行到 buffer；
2. 如果剩余空间不足，先用一次 `write()` flush 现有内容；
3. 如果单条日志大于 buffer，但没有超过最大日志长度，则绕过 buffer，直接执行一次 `write()`；
4. 不允许把同一条日志拆成两次 write。

### 9.3 定时 flush

对于 EventLoop 工作线程，在每个 EventLoop 上注册本地 timer：

```cpp
flush_at = EventLoop::current().now() + flush_interval;
```

timer callback 只 flush 当前 EventLoop 自己的 `LogContext`，不跨线程访问其他线程的 buffer。

不运行 EventLoop 的普通线程有两种选择：

- 使用同步直写 Appender；
- 显式创建 `LogContext`，在线程退出前主动 flush。

LoggerManager 不在业务线程仍运行时遍历并修改其他线程的 buffer。

### 9.4 Flush 含义

本设计中的 flush 表示：

```text
user buffer -> write() -> kernel page cache
```

默认不调用 `fsync()` 或 `fdatasync()`。如果未来增加持久化策略，应作为独立配置和管理操作，不进入普通日志热路径。

---

## 10. 文件滚动与 reopen

V1 不在 FileAppender 内执行按大小或时间滚动。推荐流程与 nginx 类似：

```text
外部 logrotate
    │
    ├── rename old log file
    └── notify application
             │
             ▼
LoggerManager::reopen_all()
             │
             ├── open new file with O_APPEND
             └── replace Appender fd
```

应用收到信号后，应通过 `SignalService` 或 EventLoop 把 reopen 转换成普通管理回调；不得在异步信号处理函数中直接解析配置、分配内存或打开文件。

多线程 reopen 必须保证 fd 替换期间正在执行的 write 仍引用有效文件。Linux 实现可以为每个 Appender 保持稳定的目标 fd，并使用 `dup3()` 原子替换其底层文件，再关闭临时 fd。

语义约定：

- 已经进入旧文件 write 的日志可以继续落到旧文件；
- 尚未 flush 的线程本地 buffer 在 reopen 后写入新文件；
- reopen 失败时继续使用旧 fd，并通过原始 stderr 路径报告错误；
- reopen 不改变 Logger 的 Level Slot 或 Appender 对象，只替换 FileAppender 内部 fd。

---

## 11. 初始化与退出

### 11.1 初始化

```text
parse application config
        │
        ▼
build LogConfig
        │
        ▼
create/open named appenders
        │
        ▼
enumerate registered LoggerHandles
        │
        ▼
group handles by unique logger name
        │
        ▼
resolve hierarchy/additivity/filter per level
        │
        ▼
calculate counts and allocate each Logger once
        │
        ▼
commit all LoggerHandle bindings
        │
        ▼
start EventLoop/EventLoopGroup
```

初始化完成前，所有 LoggerHandle 指向 bootstrap Logger，少量 bootstrap 日志直接同步写入 stderr。

初始化必须是事务性的：只有全部 Appender 打开成功、全部 Logger 创建成功后，才批量提交 LoggerHandle 绑定。任一步失败时释放本轮创建的资源，所有 Handle 继续指向 bootstrap Logger，应用不启动业务线程。

### 11.2 退出

```text
stop accepting new work
        │
        ▼
stop and join producer threads/EventLoops
        │
        ▼
flush remaining LogContexts
        │
        ▼
flush current thread
        │
        ▼
rebind LoggerHandles to bootstrap Logger
        │
        ▼
close Appender fds
        │
        ▼
release Logger arena
        │
        ▼
destroy LoggerManager
```

LoggerManager 必须比所有业务线程和 `LogContext` 活得更久。只有在工作线程全部 join 后，才能把 Handle 切回 bootstrap Logger 并销毁最终 Logger。退出阶段的晚到日志走 bootstrap stderr，或者按配置直接丢弃。

---

## 12. 错误处理

日志系统自身不得使用普通 Logger 报告 Appender 错误，否则可能递归进入失败的 Appender。

错误处理策略：

- 初始化打开文件失败：返回配置/初始化错误，应用不启动服务；
- `write()` 失败或长度不符：丢弃该条或该批日志，增加错误和丢弃计数；
- buffer 容量不足：按照最大日志长度规则截断，不动态扩容；
- reopen 失败：保留旧 fd；
- 格式化失败：输出能够生成的前缀和 `<format-error>`；
- 内部诊断：使用无递归的原始 stderr write，并进行限频。

每个 Appender 至少暴露以下统计：

```text
written_records
written_bytes
dropped_records
write_errors
reopen_errors
```

---

## 13. 关键接口草图

```cpp
class Appender {
public:
    virtual ~Appender() = default;

    // 仅在启动阶段生成 Logger 的 Level Slot 时使用。
    [[nodiscard]] virtual bool accepts(LogLevel level) const noexcept = 0;
    virtual void append(const LogEvent& event, LogContext& context) noexcept = 0;
    virtual void flush(LogContext& context) noexcept = 0;
    [[nodiscard]] virtual bool reopen() noexcept = 0;
};
```

```cpp
struct LevelTargets {
    Appender* const* first = nullptr;
    std::uint32_t count = 0;

    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

class Logger {
public:
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return !levels_[level_index(level)].empty();
    }
    [[nodiscard]] bool vlog_enabled(unsigned verbosity) const noexcept;
    void dispatch(const LogEvent& event, LogContext& context) const noexcept {
        const LevelTargets& targets = levels_[level_index(event.level)];
        for (std::uint32_t i = 0; i < targets.count; ++i) {
            targets.first[i]->append(event, context);
        }
    }

private:
    friend class LoggerManager;

    explicit Logger(std::string_view name) noexcept : name_(name) {}

    std::string_view name_;
    std::array<LevelTargets, kLogLevelCount> levels_{};
    std::uint8_t verbosity_ = 0;
};
```

```cpp
class LoggerHandle {
public:
    explicit LoggerHandle(std::string_view name) noexcept;

    [[nodiscard]] const Logger& get() const noexcept { return *logger_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    friend class LoggerManager;

    std::string_view name_;
    const Logger* logger_ = &bootstrap_logger();
    LoggerHandle* registry_next_ = nullptr;
};
```

LoggerManager 使用两遍创建流程：第一遍解析每个 level 的临时 Appender 列表并累计总 count，第二遍从 Logger arena 一次分配 `Logger + total_count * sizeof(Appender*)`，再填充尾随指针区和各 Level Slot。

```cpp
const std::size_t logger_size = align_up(sizeof(Logger), alignof(Appender*));
const std::size_t alloc_size =
    logger_size + total_count * sizeof(Appender*);

void* memory = logger_arena.alloc(
    alloc_size,
    std::max(alignof(Logger), alignof(Appender*)));
if (memory == nullptr) {
    return std::unexpected(LogInitError::OutOfMemory);
}

auto* logger = new (memory) Logger{name};
auto** storage = reinterpret_cast<Appender**>(
    static_cast<std::byte*>(memory) + logger_size);
```

Logger 和尾随指针区在初始化完成后只读。Appender 对象地址保持稳定并由 LoggerManager 持有；Logger 中的指针数组不拥有 Appender。

---

## 14. 测试与性能验收

### 14.1 功能测试

- Logger 父级和 segment 边界匹配；
- level 与 verbosity 继承；
- additivity on/off；
- 同一 Appender 去重；
- Appender level filter；
- LoggerHandle bootstrap、事务性绑定和 shutdown 回绑；
- 多个同名 LoggerHandle 共享一个最终 Logger；
- 每个 level 的 count、首指针和连续尾随数组布局；
- Logger 总分配大小溢出和 arena 分配失败；
- 禁用日志不求值消息表达式；
- 超长日志截断和控制字符转义；
- buffer 满、定时 flush、线程退出 flush；
- reopen 成功和失败；
- 初始化和 shutdown 顺序。

### 14.2 并发测试

- 多线程共享同一 `O_APPEND` fd；
- 一条日志只执行一次 write；
- buffer 模式一条日志不跨 write；
- reopen 与并发 write；
- 多 EventLoop 同时写同一组 Appender。

测试按照本设计假设，不注入成功但只写入部分数据的场景。

### 14.3 性能验收

- 禁用日志：零动态分配、零时间格式化、零 Logger name 匹配；
- 常见长度的启用日志：业务线程零动态分配；
- Logger level 判断：一次 Handle 指针读取、一次 Level Slot 读取和一次 count 判断；
- 同步直写：每个目标 Appender 每条日志最多一次 write；
- buffer 模式：按 buffer size 批量 write；
- 多线程热路径不使用全局日志 mutex。

---

## 15. 最终设计原则

1. `DEFINE_LOGGER` 创建静态 LoggerHandle，最终 Logger 在 Appender 注册完成后创建。
2. Logger 是层级业务分类，不是输出文件。
3. Appender 是具名输出目标，独占文件配置并共享 fd。
4. Logger 按 level 保存 `first + count`，所有 Appender 指针位于一次变长分配的连续尾随数组中。
5. Logger 层级、level、additivity、Appender filter 和去重全部在 Logger 创建前展开。
6. 使用方式保持 glog 风格，并确保禁用路径不求值。
7. 文件使用 `O_APPEND`，每条日志或每批 buffer 只执行一次 write。
8. 不处理一次 write 未完成，不在普通路径补写。
9. 不使用异步 writer；buffer 只负责合并 syscall。
10. buffer 归产生日志的线程或 EventLoop 所有，不跨线程 flush。
11. 文件滚动交给外部工具，系统提供 nginx 风格 reopen。
12. V1 Logger 创建后不可变，热路径不解析配置、不匹配字符串、不加全局锁。
