# Fiber Gateway

[English](README.md) | 简体中文

Fiber Gateway 是一个性能优先的 C++23 网关框架，面向反向代理、网关和异步网络服务。仓库以可复用的静态库 `fiber_lib` 为核心，`example/`、测试以及 `apps/` 下的模块共用同一套运行时与协议栈。

这是一个框架仓库，而不是只生成单一可执行程序的项目。默认构建会生成核心库、可运行示例、测试、`lite_nginx` 应用，以及 `fiber::nacos`、`fiber::prometheus` 等可选应用层库。

## 核心能力

- 基于 Linux `epoll` 的事件循环、定时器、跨线程通知和多 EventLoop 调度。
- 基于 C++23 协程的任务与异步原语，包括互斥锁、读写锁、信号、超时、等待组和带版本的 Watch。
- TCP、UDP、Unix Domain Socket、TLS Stream、DNS 解析与 DNS 缓存。
- HTTP/1.1、HTTP/2 和 HTTP/3 服务端协议栈。
- HTTP/1.1 连接池，以及 HTTP/2、HTTP/3 客户端连接。
- 内置 QUIC v1 传输实现，包含 TLS、Stream、丢包恢复、拥塞控制、发送 pacing、Connection ID、地址验证，以及系统支持时的 UDP GSO。
- HPACK、QPACK、流式请求与响应，以及通过 HTTP/1 Upgrade 或 HTTP/2/3 Extended CONNECT 实现的 WebSocket 代理。
- Nacos 客户端库及其私有的 gRPC/protobuf 传输实现。
- 脚本运行时、通用 JSON 编解码、结构化日志，以及关注分配成本的 Buffer 和内存工具。

## 仓库结构

```text
.
├── src/          # 核心框架，构建为 fiber_lib
├── example/      # 小型可运行示例和 benchmark 工具
├── apps/         # 完整应用和可选应用层库
├── tests/        # 核心 GoogleTest 测试集
├── docs/         # 稳定的模块文档
├── feature/      # 设计说明、审计记录和实现报告
├── cmake/        # 工具链、依赖和 target 辅助逻辑
└── scripts/      # 构建、互操作和 benchmark 工具
```

主要源码模块包括：

- `src/event/`：事件循环、poller、定时器和 loop group。
- `src/async/`：协程任务、调度和同步原语。
- `src/net/`：socket、listener、stream、TLS 和地址抽象。
- `src/quic/`：QUIC 传输、加密、恢复、拥塞控制和 stream。
- `src/http/`：HTTP/1.1、HTTP/2、HTTP/3、客户端、服务端和连接池。
- `src/dns/`：DNS 消息、客户端、resolver 和缓存。
- `src/common/`：错误、JSON、内存、容器和通用工具。
- `src/script/` 与 `src/http_script/`：脚本运行时和 HTTP 绑定。
- `src/log/`：logger 层级、格式化和 appender。
- `apps/nacos/`：Nacos 客户端及其私有的 gRPC/protobuf 传输实现。

## 环境要求

- Linux；当前事件后端基于 `epoll`。
- CMake 4.1 或更高版本。
- GCC 13+ 或 Clang 17+，并且标准库需提供项目所用的 C++23 能力，包括 `std::expected`。
- 首次配置通常需要网络；如果依赖源码已缓存在 `temp/_deps` 或自定义 `FIBER_DEPS_DIR` 中，则可直接复用。

未显式指定编译器或 toolchain 时，CMake 会优先选择合适的 Clang，其次选择 GCC。BoringSSL 和 protobuf-lite 是由构建系统管理的核心依赖；启用测试时使用 GoogleTest；jemalloc 是可选依赖。

## 快速开始

配置并构建所有默认 target：

```bash
cmake -S . -B build
cmake --build build
```

通过 CTest 运行发现到的 GoogleTest 用例：

```bash
ctest --test-dir build --output-on-failure
```

常见的默认构建产物包括：

```text
build/fiber_tests
build/example/http1_echo
build/example/https_echo
build/example/dns_dig
build/example/http3_benchmark_server
build/example/http3_benchmark_client
build/apps/lite_nginx
```

例如：

```bash
./build/example/http1_echo 8080
./build/example/dns_dig example.com
./build/apps/lite_nginx --check-config
./build/apps/lite_nginx
```

全部示例及 HTTP/3 benchmark 的运行方式见[示例说明](example/README.zh-CN.md)。

## 构建选项

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `FIBER_BUILD_EXAMPLES` | `ON` | 构建 `example/` 下的程序。 |
| `FIBER_BUILD_TESTS` | `ON` | 在 GoogleTest 可用时构建核心和应用测试。 |
| `FIBER_BUILD_APPS` | `ON` | 构建从 `apps/` 自动发现的模块。 |
| `FIBER_FETCH_DEPS` | `ON` | 允许获取缺失的 GoogleTest、jemalloc 等可选依赖。 |
| `FIBER_USE_JEMALLOC` | `OFF` | 最终可执行程序链接 jemalloc。 |
| `FIBER_ENABLE_HTTP3` | `ON` | 已声明的 HTTP/3 开关；当前源码列表不受该值影响，仍会包含 HTTP/3。 |
| `FIBER_ENABLE_LTO` | `ON` | 工具链支持时启用 IPO/LTO。 |
| `FIBER_ALLOW_GCC_LTO` | `OFF` | 显式启用 GCC LTO；出于稳定性考虑默认关闭。 |
| `FIBER_ENABLE_UDP_GSO` | `ON` | 系统头文件支持时编译 Linux UDP GSO。 |
| `FIBER_FORCE_TIMERFD_POLLER` | `OFF` | 强制使用 timerfd poll timeout 路径，不使用 `epoll_pwait2`。 |
| `FIBER_ENABLE_BENCHMARK_TRACE` | `OFF` | 启用仅用于 benchmark 的热点路径统计。 |
| `FIBER_USE_LIBCXX` | `OFF` | Clang 工具链使用 libc++。 |
| `FIBER_STATIC_LIBCXX` | `ON` | `FIBER_USE_LIBCXX=ON` 时静态链接 libc++ runtime。 |

`FIBER_FETCH_DEPS=OFF` 会关闭 GoogleTest 和 jemalloc 等可选依赖的 fallback 下载。BoringSSL、zlib 源码和 protobuf 仍由 `cmake/Deps.cmake` 填充；可以用 `FIBER_DEPS_DIR` 指向可复用或预先准备的源码缓存。

使用 jemalloc 的典型 Release 构建：

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_USE_JEMALLOC=ON
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

## 示例

`example/` 下的单文件程序覆盖 HTTP/HTTPS 服务端、TCP/UDP Echo、DNS 查询、Git Smart HTTP 服务，以及 HTTP/3 benchmark 客户端和服务端。它们用于提供紧凑的 API 参考和 smoke test，不代表完整生产应用的目录结构。

详细信息见 [example/README.zh-CN.md](example/README.zh-CN.md)。

## 应用与可复用模块

`apps/` 同时容纳完整应用和建立在 `fiber_lib` 之上的可选库：

- [`apps/lite_nginx`](apps/lite_nginx/README.md)：使用 nginx 风格配置的轻量反向代理。它接受 HTTP/1.1、HTTP/2 和 HTTP/3，支持 TLS 与 WebSocket tunnel，并通过连接池代理到 HTTP/1.1 上游。
- [`apps/nacos`](apps/nacos/README.md)：`fiber::nacos` 客户端库，包含认证、Nacos 2.x gRPC 传输、ConfigService、NamingService、订阅、重连和服务状态 replay。
- [`apps/prometheus`](apps/prometheus/README.md)：`fiber::prometheus` 固定 schema 指标库，使用 EventLoop-owned shard 并导出 Prometheus 文本格式。

模块目录约定和 CMake 用法见 [apps/README.md](apps/README.md)。

## 文档入口

- [示例](example/README.zh-CN.md)
- [应用目录约定](apps/README.md)
- [HTTP/1 连接池](docs/http1-connection-pool.md)
- [脚本函数签名 ABI](docs/script-function-signature-abi.md)
- [HTTP/3 客户端设计](feature/http3_client.md)
- [QUIC 客户端设计](feature/quic_client.md)
- [Nacos ConfigService 设计](feature/nacos_config_service.md)
- [Nacos NamingService 设计](feature/nacos_naming_service.md)
- [Prometheus 指标设计](feature/prometheus_cpp_metrics_design.md)

`feature/` 目录还包含持续演进的设计记录、审计结果和 benchmark 报告。除非文档明确说明当前 contract，否则应将这些内容视为工程演进记录。

## 项目状态

Fiber Gateway 仍在积极开发中。仓库已经具备较广泛的协议和运行时测试覆盖，但 API 与应用 contract 仍可能继续演进。要了解当前用法，建议从示例和各模块自己的 README 开始。

## License

本项目采用 [MIT License](LICENSE)。
