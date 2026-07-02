# Fiber

[English](README.md) | 简体中文

`Fiber` 是一个面向高性能 gateway 与服务端场景的 C++23 框架仓库。它不是“只有一个主程序”的项目，而是由一套可复用的核心库 `fiber_lib`、一组单文件示例、一个多文件应用集合，以及完整测试集组成。

这个项目的重点不只是网络 I/O 封装，而是围绕网关场景提供三类核心能力：包含 QUIC/HTTP/3 在内的服务端 HTTP 接入、面向上游的 HTTP Client 能力，以及用于增强配置灵活性和表达能力的脚本引擎支持。在这些网关能力之下，再由事件循环、协程、同步原语和高性能缓冲区等基础设施层提供支撑。

## 项目定位

- 面向网络基础设施、网关、代理和服务端组件的基础框架。
- 以静态库 `fiber_lib` 为核心，供示例、测试和应用复用。
- 仓库当前同时承担框架实现、示例验证和应用孵化三种角色。
- 默认不会产出单一 `fiber` 可执行程序；常见构建产物是库、示例、测试和 `apps/` 下的应用。

## `fiber_lib` 核心介绍

`fiber_lib` 是整个仓库的核心。它是由 `src/` 下各模块聚合而成的静态库，也是 `tests/`、`example/` 和 `apps/` 的共同依赖。它的目标是服务 gateway framework 本身，而不是只提供零散工具类或狭义的 HTTP 包装层。

从网关框架视角看，`fiber_lib` 重点提供三类核心能力。

### 1. HTTP Server 能力

网关的第一层职责是接入下游请求，因此服务端 HTTP 能力是 `fiber_lib` 的核心组成部分。

- 支持 HTTP/1.1 服务端处理链路。
- 支持 HTTP/2 服务端处理链路。
- 支持基于 QUIC/UDP listener 的 HTTP/3 服务端处理链路。
- 支持基于 BoringSSL 的 TLS 终止，并包含 HTTP/3 所需的 QUIC TLS 集成。
- 支持在 TLS listener 上通过 ALPN 协商 HTTP/1.1 或 HTTP/2，也支持 HTTP/3 使用的 `h3` ALPN。
- 提供 `HttpServer`、`Http1Server`、`Http3Server`、请求交换抽象和传输抽象，便于上层应用组装 listener、handler 和协议处理逻辑。

这部分能力对应 gateway 的入口层，负责监听、接收连接、协议协商、请求读取、响应写回，并作为后续转发链路的起点。

### 2. HTTP Client 能力

网关不仅要接收请求，还要稳定高效地访问上游服务，因此上游访问能力同样是框架重点。

- 支持 HTTP/2 Client 连接与请求交换。
- 支持 HTTP/1 Connection Pool，用于上游连接复用。
- 支持 DNS 协议、解析器、本地 resolver 和 DNS 缓存能力。
- 支持将解析、建连、连接复用和请求交换组合成完整的上游访问链路。

这部分能力主要服务于反向代理、转发、聚合以及网关内部对外访问场景。相比“一次请求一次连接”的简单 client 模式，这里的设计更强调可复用连接、解析缓存和协议级长期演进能力。

### 3. 脚本引擎能力

网关系统往往不仅需要静态配置，还需要条件判断、表达式求值和可编程扩展层。`fiber_lib` 在 `src/script/` 和 `src/common/json/` 下提供了脚本解析、IR、解释执行和运行时支持，用来为更灵活的配置和策略表达打基础。

这部分能力的意义在于：

- 为网关配置提供比纯静态 declarative 文件更强的表达能力。
- 为路由、转发、改写、条件判断和动态决策提供扩展空间。
- 为未来 app 提供统一的脚本化基础，而不是让每个 app 自己实现一套轻量 DSL。

### 基础能力

在以上三类网关核心能力之下，`fiber_lib` 还提供整个框架运行所需的基础设施层：

- `event`
  事件循环、定时器、跨线程通知和循环组。
- `coroutine` / `async`
  基于 C++23 协程的任务抽象、调度和异步原语。
- `mutex`
  包括 `Mutex`、`RWMutex`、`Signal` 和 `WaitGroup` 在内的同步组件。
- `iobuf`
  用于热点 I/O 路径的缓冲区和缓冲链抽象。
- `net`
  TCP、UDP、Unix Domain Socket、TLS 流和地址抽象。
- `quic`
  QUIC v1 传输原语、packet/frame 编解码、TLS 握手集成、丢包恢复、拥塞控制、stream 管理、地址验证 token 和 UDP endpoint 调度。
- 其他公共基础设施
  例如断言、错误模型、路由匹配、内存辅助组件和 JSON/JS 值编解码支持。

`fiber_lib` 的价值在于复用和组合。后续无论新增反向代理、网关进程、协议桥接器、调试工具还是实验性应用，通常都应优先建立在这些能力之上，而不是在 `apps/` 中重复实现框架基础设施。

## 核心模块

- `src/event/`
  事件循环、定时器、跨线程通知、poller 和 loop group。
- `src/async/`
  协程任务、线程组、互斥锁、读写锁、信号、超时和等待组。
- `src/net/`
  地址、listener、stream、datagram socket、TLS stream 和底层 fd 封装。
- `src/quic/`
  QUIC v1 传输实现、packet 处理、crypto 集成、拥塞与丢包处理、stream 队列、connection ID、token 和 UDP endpoint 分发。
- `src/dns/`
  DNS 消息、缓存、解析器、本地 resolver 支持和地址解析接口。
- `src/http/`
  HTTP/1.1、HTTP/2、HTTP/3、HPACK、QPACK、服务端/客户端交换类型、连接池和传输抽象。
- `src/common/`
  断言、I/O 错误、路由匹配、JSON、内存和通用基础设施。
- `src/script/`
  词法、语法、IR、解释执行和运行时支持。
- `example/`
  单文件示例，聚焦最小可运行用法。
- `apps/`
  多文件可运行应用目录。当前包含 `lite_nginx`，后续也用于承载新的应用。
- `tests/`
  基于 GoogleTest 的测试集合。当前仓库包含 93 个 `*Test.cpp` 文件，并包含 QUIC 和 HTTP/3 覆盖。

## 仓库结构

```text
.
├── src/                 # 框架源码，构建为 fiber_lib
├── example/             # 单文件示例
├── apps/                # 多文件应用
├── tests/               # GoogleTest / CTest
├── docs/                # 设计和模块文档
├── feature/             # 功能说明和设计草案
├── cmake/               # 构建脚本和依赖处理
└── scripts/             # 辅助脚本
```

## 示例与应用

### `example/`

`example/` 提供最小可运行程序，用来展示 `fiber_lib` 的典型使用模式。顶层 README 只保留简要入口，详细说明见 [example/README.md](example/README.md) 和 [example/README.zh-CN.md](example/README.zh-CN.md)。

### 示例程序

默认会构建以下示例：

- `http1_echo`
- `https_echo`
- `tcp_echo`
- `udp_echo`
- `dns_dig`
- `git_http_repo_server`

### `apps/`

`apps/` 是建立在 `fiber_lib` 之上的多文件应用目录。与 `example/` 相比，`apps/` 面向的是完整应用，而不是最小演示。

这里的应用通常会包含：

- 独立的目录结构和 `CMakeLists.txt`
- 应用自己的配置、运行时装配和业务模块
- 更接近生产使用方式的命令行入口
- 更清晰的内部边界，例如 `config/`、`runtime/`、`proxy/`、`upstream/`
- 除顶层通用测试之外的应用级测试

`apps/` 不是只为当前 `lite_nginx` 预留的目录，而是框架之上的应用孵化层。如果后续仓库继续发展新的 gateway、proxy、debugger、protocol bridge 或实验性服务，它们通常都应以并列子目录形式放在这里。

这让仓库形成了比较清晰的三层结构：

- `fiber_lib`
  负责公共框架能力。
- `example/`
  负责最小 API 样板和可运行参考程序。
- `apps/`
  负责真实应用和应用级架构沉淀。

### 当前应用

- `apps/lite_nginx`
  一个轻量级反向代理，采用 nginx 风格配置语法，但 V1 特性集刻意保持更小、更明确，并不追求完全兼容。TLS listener 可以协商 HTTP/1.1 或 HTTP/2；配置了 `http3`/`quic` 的 listener 还会绑定 UDP，用于下游 HTTP/3 over QUIC，并通告 `Alt-Svc`。

## 环境要求

- CMake 4.1 或更高版本
- 支持 C++23 的编译器
- GCC 13+ 或 Clang 17+

项目会在配置阶段检查编译器能力。

## 依赖

### 默认依赖

- BoringSSL：用于 TLS、HTTPS 以及 QUIC/HTTP/3 crypto 相关能力，默认会在需要时自动获取。

### 可选依赖

- GoogleTest：用于测试；在 `FIBER_FETCH_DEPS=ON` 时可自动下载。
- jemalloc：可选运行时分配器。

## 快速开始

### 1. 配置并构建

典型开发构建：

```bash
cmake -S . -B build
cmake --build build
```

通常会生成：

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
  不构建 `example/` 下的示例。
- `-DFIBER_BUILD_APPS=OFF`
  不构建 `apps/` 下的应用。
- `-DFIBER_BUILD_TESTS=OFF`
  不构建测试目标。
- `-DFIBER_FETCH_DEPS=OFF`
  关闭自动下载第三方依赖，要求系统已安装依赖。
- `-DFIBER_USE_JEMALLOC=ON`
  将最终可执行程序链接到 jemalloc。
- `-DFIBER_ENABLE_LTO=ON`
  在工具链支持时启用 LTO/IPO。
- `-DFIBER_ALLOW_GCC_LTO=ON`
  显式允许 GCC 使用 LTO；默认关闭以规避已知不稳定问题。

示例：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DFIBER_USE_JEMALLOC=ON
cmake --build build-release
ctest --test-dir build-release
```

## 设计方向

- 性能优先，关注热点路径上的分配行为。
- 倾向显式状态和清晰生命周期，而不是大面积可空状态和隐式共享。
- 通过 `IoBuf`、连接池、事件循环和协程控制流构建高吞吐网络路径。
- 通过示例、测试和应用共同验证框架行为。

## 文档入口

- [HTTP/1 Connection Pool](docs/http1-connection-pool.md)
- [lite-nginx Requirements](apps/lite_nginx/README.md)
- `feature/` 目录包含多个领域的设计说明，例如 HTTP/2、脚本、事件循环、流和 socket。

## 当前状态

仓库已经具备较完整的网络基础设施和测试覆盖，但更适合被视为一个持续演进中的 framework codebase，而不是已经完全封装好的通用 SDK。实际使用中，理解接口最快的方式通常是阅读示例和 `apps/lite_nginx`。

## License

本项目采用 [MIT License](LICENSE)。
