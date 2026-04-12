# Examples

`example/` 目录放的是基于 `fiber_lib` 的最小可运行示例。它们的目标是演示某一类能力如何被组合使用，而不是提供完整应用结构。

这些示例通常具有以下特点：

- 单文件实现，便于快速阅读。
- 聚焦单一主题，避免引入过多应用层封装。
- 适合作为 API 使用样板和轻量 smoke test。

## 示例列表

- `http1_echo.cpp`
  最小 HTTP/1.1 服务端示例，展示事件循环、请求体读取和响应写回。
- `https_echo.cpp`
  HTTPS 服务端示例，展示 TLS 接入和安全连接处理。
- `tcp_echo.cpp`
  TCP Echo 示例，展示基础流式连接读写。
- `udp_echo.cpp`
  UDP Echo 示例，展示数据报收发。
- `dns_dig.cpp`
  DNS 查询示例，展示解析器、地址解析和结果处理。
- `git_http_repo_server.cpp`
  较完整的 HTTP 服务端示例，展示更复杂的协议处理和服务逻辑。
- `http3_demo_lsquic.cpp`
  基于 `lsquic` 的 HTTP/3 实验性示例；只有在相关依赖可用时才会构建。

## 构建

默认情况下，顶层构建会同时构建 `example/` 下的示例：

```bash
cmake -S .. -B ../build
cmake --build ../build
```

如果只想关闭示例构建：

```bash
cmake -S .. -B ../build -DFIBER_BUILD_EXAMPLES=OFF
```

## 运行

示例可执行文件默认输出在顶层构建目录，例如：

```bash
../build/http1_echo
../build/https_echo
../build/dns_dig example.com
```
