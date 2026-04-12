# Fiber

`Fiber` 是一个面向高性能网络网关与服务端场景的 C++23 框架仓库。它不是“只有一个主程序”的项目，而是由一套可复用的底层库 `fiber_lib`、一组单文件示例、一个多文件应用集合，以及完整测试集组成。

仓库当前覆盖的能力包括事件循环、协程异步原语、TCP/UDP/Unix/TLS 网络通信、DNS 解析与缓存、HTTP/1.1 与 HTTP/2、连接池、轻量脚本运行时，以及基于这些能力构建的反向代理应用。整体设计偏性能优先，强调显式状态、低分配开销和可组合的基础设施层。

## 项目定位

- 面向网络基础设施、网关、代理和服务端组件的底层框架。
- 以静态库 `fiber_lib` 为核心，供示例程序、测试和应用复用。
- 当前仓库同时承担“框架实现 + 示例验证 + 应用孵化”的角色。
- 不提供默认安装后的单一可执行程序；构建结果通常是库、示例、测试和 `apps/` 下的应用。

## `fiber_lib` 核心介绍

`fiber_lib` 是整个仓库的基础能力层，也是 `example/`、`tests/` 与 `apps/` 的共同依赖。它的职责不是包装一个狭义的 HTTP API，而是提供一套可以自由组合的网络运行时与协议组件，使上层程序可以按需拼装事件循环、异步任务、网络 I/O、解析器、协议栈与内存模型。

从结构上看，`fiber_lib` 更接近“面向网关/服务端的基础设施库”，而不是“一个固定框架模板”。你可以只使用其中的事件循环和 TCP/TLS 流来写自定义服务，也可以叠加 DNS、HTTP、连接池、脚本运行时等模块构建更复杂的代理、中间层和应用服务。

它当前主要承担几类能力：

- 运行时能力
  提供 `EventLoop`、`EventLoopGroup`、定时器、通知队列和协程调度基础设施。
- 并发与异步原语
  提供 `Task`、`DetachedTask`、`Spawn`、`WaitGroup`、`Mutex`、`RWMutex`、`Signal`、`Timeout` 等组件。
- 网络能力
  提供 TCP、UDP、Unix Socket、TLS 流、监听器、地址和底层 fd 封装。
- 协议与网关能力
  提供 DNS、HTTP/1.1、HTTP/2、HPACK、客户端交换、服务端处理和连接池能力。
- 数据与内存能力
  提供 `IoBuf`、缓冲链、分配器、JSON/JS 值编解码等性能敏感组件。
- 可扩展执行能力
  提供脚本解析、IR、解释执行与运行时支持，便于后续叠加更高层的动态能力。

这一层的价值在于复用。后续无论新增反向代理、调试工具、网关进程，还是协议实验性应用，通常都应优先建立在 `fiber_lib` 之上，而不是在 `apps/` 中重复实现底层能力。

## 核心特性

- 基于自定义事件循环、定时器队列和轮询器的事件驱动模型。
- 基于 C++23 协程的异步任务体系，包含 `Task`、`DetachedTask`、`Spawn` 等基础设施。
- TCP、UDP、Unix Domain Socket、TLS 流封装。
- DNS 协议、缓存、本地解析器和地址解析器。
- HTTP/1.1 服务端与客户端能力。
- HTTP/2 连接、流、HPACK 编解码、发送调度与相关测试覆盖。
- HTTP/1 连接池与上游复用能力。
- 面向热点路径的内存组件，例如 `IoBuf`、缓冲链、分配器和池化设施。
- JSON/JS 值编码与一个轻量脚本子系统。
- 基于现有 HTTP 栈实现的 `lite-nginx` 反向代理应用。

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

### `example/` 的作用

`example/` 用来放单文件、低依赖、可直接运行的样例程序。它的目标不是做“完整产品”，而是作为最短路径的用法说明，帮助开发者快速理解某一组 API 应该怎样组合。

这类示例通常有几个特征：

- 单文件实现，方便直接阅读和复制改造。
- 聚焦单一主题，例如 Echo、DNS、HTTP 服务或特定协议演示。
- 尽量避免引入复杂配置、目录结构和业务封装。
- 同时承担文档样板和轻量 smoke test 的作用。

如果你想快速理解 `fiber_lib` 的实际用法，优先看 `example/` 往往比直接读底层模块更高效。

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

这些示例大致覆盖了以下用途：

- `http1_echo`
  最小 HTTP/1.1 服务端示例，展示事件循环、请求读取和响应写回。
- `https_echo`
  展示 TLS 服务端与 HTTPS 处理链路。
- `tcp_echo`
  展示基础 TCP 流读写。
- `udp_echo`
  展示 UDP 数据报收发。
- `dns_dig`
  展示 DNS 解析能力、地址解析结果和相关运行时组件。
- `git_http_repo_server`
  展示更复杂的 HTTP 服务端用法和真实协议处理场景。
- `http3_demo_lsquic`
  展示基于 `lsquic` 的 HTTP/3 实验性接入能力。

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
