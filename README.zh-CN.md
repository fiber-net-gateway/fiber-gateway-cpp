# Fiber Gateway

[English](README.md) | 简体中文

Fiber Gateway 是一个性能优先的 C++23 网关框架，面向反向代理、网关和异步网络服务。仓库以可复用的静态库 `fiber_lib` 为核心，`example/`、测试以及 `apps/` 下的模块共用同一套运行时与协议栈。

这是一个框架仓库，而不是只生成单一可执行程序的项目。默认构建会生成核心库、可运行示例、测试、`lite_nginx` 应用，以及 `fiber::nacos`、`fiber::cat`、`fiber::prometheus` 等可复用组件。

## 核心能力

- 基于 Linux `epoll` 的事件循环、定时器、跨线程通知和多 EventLoop 调度。
- 基于 C++23 协程的任务与异步原语，包括互斥锁、读写锁、信号、超时、等待组和带版本的 Watch。
- TCP、UDP、Unix Domain Socket、TLS Stream、DNS 解析与 DNS 缓存。
- HTTP/1.1、HTTP/2 和 HTTP/3 服务端协议栈。
- HTTP/1.1 连接池，以及 HTTP/2、HTTP/3 客户端连接。
- 内置 QUIC v1 传输实现，包含 TLS、Stream、丢包恢复、拥塞控制、发送 pacing、Connection ID、地址验证，以及系统支持时的 UDP GSO。
- HPACK、QPACK、流式请求与响应，以及通过 HTTP/1 Upgrade 或 HTTP/2/3 Extended CONNECT 实现的 WebSocket 代理。
- Nacos 客户端库及其私有的 gRPC/protobuf 传输实现。
- 面向网关定制的类 JS 字节码引擎，支持编译期宿主绑定、请求级 GC，以及与原生协程协作的 HTTP API。
- 通用 JSON 编解码、结构化日志，以及关注分配成本的 Buffer 和内存工具。

## 面向网关定制的脚本引擎

网关策略的变化速度往往快于原生数据平面的迭代速度。静态配置语言适合组装监听器和上游，
但面对条件路由、请求检查、请求头与载荷转换、灰度决策和多步上游调用时会迅速变得笨重。
把所有策略都写成 C++ 可以保持热点路径高效，却也意味着每次策略调整都需要重新构建和部署。
Fiber 的脚本引擎有意充当这两个极端之间的窄层：由配置选择并编译策略，请求仍然在同一个
C++ 网关进程内执行。

该运行时是项目内置的类 JS 字节码解释器，并非 ECMAScript 实现。它提供整数与浮点数、
字符串、二进制值、数组、对象、模板字符串、控制流、异常，以及面向网关的标准库。
HTTP 绑定提供请求数据读取、响应构造、路由与连接常量，以及通过指令绑定上游的
`request()` 和流式 `proxyPass()` 操作：

```javascript
directive backend = http "@api";

if ($header.x_canary == "1") {
    return backend.proxyPass({
        headers: {"X-Route": "canary"}
    });
}

resp.sendJson(200, {status: "ready", path: $req.path});
```

脚本在加载配置时完成编译，因此未知函数、参数数量错误、不可用的路由常量和无效指令都会
在开始处理流量前暴露。生成的只读字节码可以跨请求复用；执行状态和 GC 值则归属于请求独占的
`GcHeap`。宿主通过 `Library` 显式注册函数，使脚本可见的能力边界保持精简，也让 C++ 与脚本
之间的 ABI 清晰可控。

本项目需要专用脚本引擎，是因为嵌入通用 JavaScript 运行时会同时引入另一套垃圾回收器与
对象模型、更大的依赖规模和 ABI 接口面，以及大量与网关策略无关的语言能力。它还要求采用第二套
事件循环/异步模型，或者为 Fiber 调度器维护一层复杂桥接，这与 Fiber 的协程所有权和分配模型
并不契合。专用引擎则把异步宿主函数直接编译为专用操作码：VM 可以直接挂起在
`fiber::async::Task` 上并恢复执行，脚本无需引入 `Promise` 或 `await` 语法。执行结果明确区分
返回值、可捕获的脚本异常，以及宿主/运行时中止，让网关能够有意识地映射不同失败。

因此，这套引擎并不试图替代浏览器或 Node.js 运行时。它的价值在于语言能力范围、字节码、内存
所有权和 HTTP 能力可以与本项目的性能及生命周期要求共同演进。显式能力模型减少了暴露给
脚本的机制，但它本身并不构成安全沙箱；宿主仍需落实请求大小、超时、脚本信任边界和资源限制等
策略。完整的语言、标准库、HTTP API 与 C++ 嵌入约定见
[脚本模块使用指南](docs/script-guide.zh-CN.md)。

## 仓库结构

```text
.
├── include/fiber # fiber_lib 公共头文件
├── src/          # fiber_lib 核心实现
├── example/      # 小型可运行示例和 benchmark 工具
├── apps/         # 完整应用和可选应用层库
├── tests/        # 核心 GoogleTest 测试集
├── docs/         # 稳定的模块文档
├── feature/      # 设计说明、审计记录和实现报告
├── cmake/        # 工具链、依赖和 target 辅助逻辑
└── scripts/      # 构建、互操作和 benchmark 工具
```

核心模块的公共头文件位于 `include/fiber/`，实现和私有头文件位于对应的 `src/`
目录。只有 `include/fiber/` 会传递给使用方；`src/` 仅供核心库和白盒测试私有使用：

- `event/`：事件循环、poller、定时器和 loop group。
- `async/`：协程任务、调度和同步原语。
- `net/`：socket、listener、stream、TLS 和地址抽象。
- `quic/`：QUIC 传输、加密、恢复、拥塞控制和 stream。
- `http/`：HTTP/1.1、HTTP/2、HTTP/3、客户端、服务端和连接池。
- `dns/`：DNS 消息、客户端、resolver 和缓存。
- `common/`：错误、JSON、内存、容器和通用工具。
- `script/` 与 `http_script/`：脚本运行时和 HTTP 绑定。
- `log/`：logger 层级、格式化和 appender。
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
| `FIBER_BUILD_APPS` | `ON` | 构建从 `apps/` 自动发现的可运行应用。 |
| `FIBER_BUILD_NACOS` | `FIBER_BUILD_APPS` 的初始值 | 构建可复用的 `fiber::nacos` 组件。 |
| `FIBER_BUILD_CAT` | `FIBER_BUILD_APPS` 的初始值 | 构建可复用的 `fiber::cat` 组件。 |
| `FIBER_BUILD_PROMETHEUS` | `FIBER_BUILD_APPS` 的初始值 | 构建可复用的 `fiber::prometheus` 组件。 |
| `FIBER_BUILD_CAT_DEMO` | `OFF` | 构建 CAT 客户端 demo。 |
| `FIBER_BUILD_PROMETHEUS_BENCHMARK` | `OFF` | 构建 Prometheus 记录路径 benchmark。 |
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

所有下载的源码归档都会进行 SHA-256 校验。受限网络或下游构建可以通过
`FIBER_<DEPENDENCY>_URL` 和 `FIBER_<DEPENDENCY>_SHA256` cache 变量覆盖
BoringSSL、zlib、protobuf、GoogleTest 与 jemalloc 的下载地址和校验值，无需修改本仓库源码。

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

Nacos、CAT 和 Prometheus 的源码目前仍位于 `apps/`，但该目录不是下游 CMake API。FetchContent
使用者通过顶层选项启用组件，并链接稳定目标；启用组件不会隐式添加 demo 或 benchmark：

```cmake
include(FetchContent)
FetchContent_Declare(
    fiber_gateway_cpp
    GIT_REPOSITORY https://github.com/fiber-net-gateway/fiber-gateway-cpp.git
    GIT_TAG <固定版本>)
set(FIBER_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(FIBER_BUILD_NACOS ON CACHE BOOL "" FORCE)
set(FIBER_BUILD_CAT ON CACHE BOOL "" FORCE)
set(FIBER_BUILD_PROMETHEUS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fiber_gateway_cpp)
target_link_libraries(my_gateway PRIVATE fiber::nacos fiber::cat fiber::prometheus)
```

应用与可复用组件包括：

- [AI Gateway](https://github.com/fiber-net-gateway/ai-gateway)：`ai-server` 的权威开发仓库；本仓库已删除旧实现，仅保留[迁移说明](apps/ai-server/README.md)与 Git 历史。
- [`apps/lite_nginx`](apps/lite_nginx/README.md)：使用 nginx 风格配置的轻量反向代理。它接受 HTTP/1.1、HTTP/2 和 HTTP/3，支持 TLS 与 WebSocket tunnel，并通过连接池代理到 HTTP/1.1 上游。
- [`apps/nacos`](apps/nacos/README.md)：`fiber::nacos` 客户端库，包含认证、Nacos 2.x gRPC 传输、ConfigService、NamingService、订阅、重连和服务状态 replay。
- [`apps/cat`](apps/cat/README.md)：面向 CAT 3.0 的 `fiber::cat` 原生客户端库。
- [`apps/prometheus`](apps/prometheus/README.md)：`fiber::prometheus` 固定 schema 指标库，使用 EventLoop-owned shard 并导出 Prometheus 文本格式。

模块目录约定和 CMake 用法见 [apps/README.md](apps/README.md)。

## 文档入口

- [示例](example/README.zh-CN.md)
- [应用目录约定](apps/README.md)
- [脚本模块使用指南](docs/script-guide.zh-CN.md)（[English](docs/script-guide.md)）
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
