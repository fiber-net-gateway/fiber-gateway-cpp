# Examples

[English](README.md) | 简体中文

`example/` 目录放的是基于 `fiber_lib` 的最小可运行示例。它们的目标是展示某一类能力如何被组合和使用，而不是提供完整应用结构。

这些示例通常具有以下特点：

- 单文件实现，便于快速阅读。
- 一次只聚焦一个主题。
- 尽量减少应用层结构和配置。
- 既可以作为 API 使用参考，也可以作为轻量 smoke test。

## 示例列表

- `http1_echo.cpp`
  最小 HTTP/1.1 服务端示例，展示事件循环、请求体读取和响应写回。
- `https_echo.cpp`
  HTTPS 服务端示例，展示 TLS 接入、HTTP/1.1、HTTP/2 以及 QUIC 上的 HTTP/3。
- `http3_benchmark_client.cpp`
  HTTP/3 压测客户端，支持 closed-loop、固定 RPS、多 QUIC 连接/stream、响应校验和延迟汇总。
- `http3_benchmark_server.cpp`
  低开销 HTTP/3 benchmark server，提供固定大小响应和请求体 echo 接口。
- `tcp_echo.cpp`
  TCP Echo 示例，展示基础流式读写行为。
- `udp_echo.cpp`
  UDP Echo 示例，展示数据报收发流程。
- `dns_dig.cpp`
  DNS 查询示例，展示 resolver 使用、地址解析和结果处理。
- `git_http_repo_server.cpp`
  更完整的 HTTP 服务端示例，展示更复杂的协议处理和服务逻辑。

## 构建

默认情况下，顶层构建也会构建 `example/` 下的示例：

```bash
cmake -S .. -B ../build
cmake --build ../build
```

如果想关闭示例构建：

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

使用仓库 demo 证书启动 HTTP/3 benchmark server 和 client：

```bash
../build/example/http3_benchmark_server 18443 2 ../build/http3-demo/cert.pem ../build/http3-demo/key.pem

../build/example/http3_benchmark_client \
  https://localhost:18443/bench/1k \
  --connect-to 127.0.0.1:18443 \
  --connections 4 --streams 16 \
  --warmup 1s --duration 10s \
  --expect-status 200 --expect-bytes 1024 \
  --insecure
```

固定总请求速率使用 `--mode rate --rps N`，完整选项见 `--help`。内部客户端适合开发和回归；
正式跨实现对比仍应使用 `scripts/benchmark/http3/` 下的独立客户端。
压测客户端默认关闭 QUIC send pacing；使用 `--pacing on` 可做延迟和突发流量对照。
若目标端在突发流量下出现连接关闭或重传，应以 `--pacing on` 复测；该默认值不适用于生产发送端。
