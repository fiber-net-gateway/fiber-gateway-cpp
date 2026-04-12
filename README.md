# Fiber

`Fiber` 是一个面向高性能 gateway 与服务端场景的 C++23 框架仓库。它不是“只有一个主程序”的项目，而是由一套可复用的底层库 `fiber_lib`、一组单文件示例、一个多文件应用集合，以及完整测试集组成。

这个项目的重点不只是网络 I/O 封装，而是围绕网关场景提供一套可组合的核心能力：服务端 HTTP 接入、面向上游的 HTTP Client 能力，以及用于增强配置灵活性和可编程表达能力的脚本引擎支持。在这些能力之下，再由事件循环、协程、锁、缓冲区等基础设施层提供支撑。

## 项目定位

- 面向网络基础设施、网关、代理和服务端组件的底层框架。
- 以静态库 `fiber_lib` 为核心，供示例程序、测试和应用复用。
- 当前仓库同时承担“框架实现 + 示例验证 + 应用孵化”的角色。
- 不提供默认安装后的单一可执行程序；构建结果通常是库、示例、测试和 `apps/` 下的应用。

## `fiber_lib` 核心介绍

`fiber_lib` 是整个仓库的核心。它是 `src/` 下各模块聚合而成的静态库，也是 `tests/`、`example/` 和 `apps/` 的共同依赖。这个库的目标是服务 gateway 框架本身，而不是只提供零散的工具类或一个狭义的 HTTP 包装层。

从网关框架视角看，`fiber_lib` 重点提供三类核心能力。

### 1. HTTP Server 能力

网关的第一层职责是接入下游请求，因此服务端能力是 `fiber_lib` 的核心组成部分。

- 支持 HTTP/1.1 服务端处理链路。
- 支持 HTTP/2 服务端处理链路。
- 支持 TLS 终止，基于 BoringSSL 构建 HTTPS 能力。
- 支持在 TLS 场景下通过 ALPN 协商 HTTP/1.1 或 HTTP/2。
- 提供 `HttpServer`、`Http1Server`、请求交换与传输抽象，便于上层应用组装 listener、handler 和协议处理逻辑。

这部分能力对应 gateway 的入口层，负责监听、接收、协议协商、请求读取、响应写回以及后续转发链路的起点。

### 2. HTTP Client 能力

网关不仅要接收请求，还要稳定地访问上游服务，因此上游访问能力同样是框架重点。

- 支持 HTTP/2 Client 连接与请求交换。
- 支持 HTTP/1 Connection Pool，用于上游连接复用。
- 支持 DNS 协议、解析器、本地 resolver 与缓存能力。
- 支持把解析、连接、复用和请求交换组合成完整的上游访问链路。

这部分能力主要服务于反向代理、转发、聚合和网关内部对外访问场景。相比“单次请求即连即断”的简单 client，这里的设计更强调可复用连接、解析缓存和协议级长期演进能力。

### 3. 脚本引擎能力

网关场景往往不仅需要静态配置，还需要更强的条件判断、表达式求值和可编程扩展能力。`fiber_lib` 在 `src/script/` 和 `src/common/json/` 下提供了脚本解析、IR、解释执行和运行时支持，用来为更灵活的配置和策略表达打基础。

这部分能力的意义在于：

- 为网关配置引入更强的表达能力，而不局限于纯静态 declarative 配置。
- 为路由、转发、改写、条件判断和动态决策提供可扩展空间。
- 为后续 app 在配置层之上增加脚本化能力提供统一基础，而不是在每个 app 中各自实现一套轻量 DSL。

### 基础能力

在以上三类核心能力之下，`fiber_lib` 还提供支撑整个框架运行的基础设施层：

- `event`
  事件循环、定时器、跨线程通知和循环组。
- `coroutine` / `async`
  基于 C++23 协程的任务体系、调度和异步原语。
- `mutex`
  包括 `Mutex`、`RWMutex`、`Signal`、`WaitGroup` 等并发同步组件。
- `iobuf`
  用于热点路径数据收发的缓冲区与缓冲链抽象。
- `net`
  TCP、UDP、Unix Socket、TLS 流和地址封装。
- 其他通用支撑
  例如断言、错误模型、路由匹配、内存分配和 JSON/JS 值编解码。

`fiber_lib` 的价值在于复用和组合。后续无论新增反向代理、网关进程、协议桥接器，还是调试与实验性应用，通常都应优先建立在这套核心能力之上，而不是在 `apps/` 中重复实现底层框架。

## 核心模块

- `src/event/`
  事件循环、定时器、跨线程通知、轮询器和循环组。
- `src/async/`
  协程任务、线程组、互斥锁、读写锁、信号、超时和等待组。
- `src/net/`
  地址、监听器、流、数据报、TLS 流和底层 fd 封装。
- `src/dns/`
  DNS 消息、缓存、解析器、本地 resolver 和地址解析接口。
- `src/http/`
  HTTP/1.1、HTTP/2、HPACK、服务端、客户端交换、连接池和传输抽象。
- `src/common/`
  断言、I/O 错误、路由匹配、JSON、内存与通用基础设施。
- `src/script/`
  词法/语法分析、IR、解释执行和运行时支持。
- `example/`
  单文件示例，强调最小可运行和 API 用法演示。
- `apps/`
  多文件可运行应用目录，当前包含 `lite_nginx`，后续可继续扩展新的应用。
- `tests/`
  GoogleTest 测试，当前仓库包含 64 个 `*Test.cpp` 测试文件。

## 仓库结构

```text
.
├── src/                 # 框架源码，生成静态库 fiber_lib
├── example/             # 单文件示例程序
├── apps/                # 多文件应用
├── tests/               # GoogleTest/CTest
├── docs/                # 设计与模块文档
├── feature/             # 功能设计草案与说明
├── cmake/               # 构建脚本与依赖处理
└── scripts/             # 辅助脚本
```

## 已有示例与应用

### `example/`

`example/` 提供最小可运行示例，用来展示 `fiber_lib` 的典型用法。顶层 README 只保留简要入口，具体示例说明见 [example/README.md](example/README.md)。

### 示例程序

默认会构建以下示例：

- `http1_echo`
- `https_echo`
- `tcp_echo`
- `udp_echo`
- `dns_dig`
- `git_http_repo_server`

当 `lsquic` 及其依赖可用时，还会额外构建：

- `http3_demo_lsquic`

### `apps/` 的作用

`apps/` 是多文件应用目录，用来承载建立在 `fiber_lib` 之上的真实可运行程序。与 `example/` 相比，`apps/` 更偏向“完整应用”而不是“最小演示”。

这里的应用通常会包含：

- 独立的目录结构与 `CMakeLists.txt`
- 自己的配置、运行时装配和业务模块
- 面向实际使用的命令行入口
- 更清晰的边界划分，例如 `config/`、`runtime/`、`proxy/`、`upstream/`
- 自己的测试，而不仅仅依赖顶层通用测试

`apps/` 也不是只为当前的 `lite_nginx` 准备的固定目录，它更像是框架之上的应用孵化层。后续如果仓库继续发展新的网关、代理、调试器、协议桥接器或实验性服务，都应该以独立子目录的形式放在这里，与已有应用并列存在。

这使仓库形成了比较清晰的三层关系：

- `fiber_lib`
  负责底层通用能力。
- `example/`
  负责最小化演示和 API 用法样板。
- `apps/`
  负责面向真实场景的可运行程序和应用级架构沉淀。

### 当前应用

- `apps/lite_nginx`
  一个轻量级反向代理，采用 nginx 风格配置语法，但刻意保持较小、明确且不完全兼容的 V1 能力集。

## 环境要求

- CMake 4.1 或更高版本
- 支持 C++23 的编译器
- GCC 13+，或 Clang 17+

项目会在配置阶段检查编译器是否具备可用的 C++23 支持。

## 依赖说明

### 默认依赖

- BoringSSL：用于 TLS/HTTPS 相关能力，默认可由 CMake 自动获取。

### 可选依赖

- GoogleTest：用于测试；本地未安装时可在开启 `FIBER_FETCH_DEPS=ON` 时自动下载。
- zlib：为 `lsquic` 和 HTTP/3 示例提供支持。
- lsquic：启用 `http3_demo_lsquic` 示例。
- jemalloc：可选地用于最终可执行程序的运行时分配器。

## 快速开始

### 1. 配置并构建

开发环境常用构建：

```bash
cmake -S . -B build
cmake --build build
```

这一步通常会生成：

- `build/fiber_tests`
- `build/http1_echo`
- `build/https_echo`
- `build/tcp_echo`
- `build/udp_echo`
- `build/dns_dig`
- `build/git_http_repo_server`
- `build/apps/lite_nginx`

### 2. 运行测试

```bash
ctest --test-dir build
```

### 3. 运行示例

HTTP/1 Echo：

```bash
./build/http1_echo 8080
```

HTTPS Echo：

```bash
./build/https_echo
```

DNS 示例：

```bash
./build/dns_dig example.com
```

### 4. 运行应用

启动 `lite_nginx`：

```bash
./build/apps/lite_nginx
```

校验配置：

```bash
./build/apps/lite_nginx --check-config
```

## 常用构建选项

- `-DFIBER_BUILD_EXAMPLES=OFF`
  不构建 `example/` 下的示例程序。
- `-DFIBER_BUILD_APPS=OFF`
  不构建 `apps/` 下的应用。
- `-DFIBER_BUILD_TESTS=OFF`
  不构建测试目标。
- `-DFIBER_FETCH_DEPS=OFF`
  禁止 CMake 自动下载第三方依赖，要求系统已具备所需依赖。
- `-DFIBER_USE_JEMALLOC=ON`
  将最终可执行程序链接到 jemalloc。
- `-DFIBER_ENABLE_LTO=ON`
  为支持的编译器启用 LTO/IPO。
- `-DFIBER_ALLOW_GCC_LTO=ON`
  显式允许 GCC 构建启用 LTO；默认关闭以规避已知不稳定问题。

示例：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DFIBER_USE_JEMALLOC=ON
cmake --build build-release
ctest --test-dir build-release
```

## 设计取向

- 性能优先，尽量减少热点路径中的动态分配。
- 倾向显式状态与明确生命周期，而不是隐式共享和宽泛可空状态。
- 通过 `IoBuf`、连接池、事件循环和协程组合构建高吞吐网络路径。
- 用示例、测试和应用共同约束基础库行为，而不是仅靠接口声明。

## 文档入口

- [HTTP/1 Connection Pool](docs/http1-connection-pool.md)
- [lite-nginx Requirements](apps/lite_nginx/README.md)
- `feature/` 目录下包含多个能力设计草案，例如 HTTP/2、脚本、事件循环、流和套接字相关说明。

## 当前状态

这个仓库已经具备较完整的网络基础设施与测试覆盖，但整体更适合视作持续演进中的框架代码库，而不是一个已经稳定发布并封装完善的通用 SDK。阅读示例程序和 `apps/lite_nginx` 往往是理解接口用法的最快入口。

## License

本项目采用 [MIT License](LICENSE)。
