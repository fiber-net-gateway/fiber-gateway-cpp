#ifndef FIBER_HTTP_HTTP3_ENGINE_H
#define FIBER_HTTP_HTTP3_ENGINE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/SocketAddress.h"
#include "TlsOptions.h"

namespace fiber::event {
class EventLoop;
}

namespace fiber::net {
class UdpSocket;
}

namespace fiber::http {

class Http3Engine;
struct Http3EngineAccess;

class Http3Connection {
public:
    Http3Connection() noexcept = default;

    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept { return local_addr_; }
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept { return remote_addr_; }
    [[nodiscard]] bool valid() const noexcept { return native_ != nullptr; }

    void close() noexcept;

private:
    Http3Connection(void *native, net::SocketAddress local_addr, net::SocketAddress remote_addr) noexcept;

    void *native_ = nullptr;
    net::SocketAddress local_addr_{};
    net::SocketAddress remote_addr_{};

    friend struct Http3EngineAccess;
};

class Http3Stream {
public:
    Http3Stream() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return native_ != nullptr; }
    [[nodiscard]] Http3Connection *connection() const noexcept { return conn_; }

    common::IoResult<void> want_read(bool enabled) noexcept;
    common::IoResult<void> want_write(bool enabled) noexcept;
    common::IoResult<std::size_t> read(void *buf, std::size_t len) noexcept;
    common::IoResult<std::size_t> write(const void *buf, std::size_t len) noexcept;
    common::IoResult<void> shutdown_write() noexcept;
    void close() noexcept;

private:
    Http3Stream(void *native, Http3Connection *conn) noexcept;

    void *native_ = nullptr;
    Http3Connection *conn_ = nullptr;

    friend struct Http3EngineAccess;
};

struct Http3EngineAccess {
    static Http3Connection make_connection(void *native, net::SocketAddress local_addr,
                                           net::SocketAddress remote_addr) noexcept {
        return {native, std::move(local_addr), std::move(remote_addr)};
    }

    static Http3Stream make_stream(void *native, Http3Connection *conn) noexcept { return {native, conn}; }

    static void clear(Http3Connection &conn) noexcept { conn.native_ = nullptr; }
    static void clear(Http3Stream &stream) noexcept { stream.native_ = nullptr; }
};

struct Http3EngineOptions {
    std::chrono::milliseconds max_wait{200};
    std::chrono::seconds idle_timeout{70};
    std::uint32_t max_bidirectional_streams = 128;
    bool enable_ecn = true;
};

struct Http3EngineHandlerOps {
    void (*on_connection_opened)(void *ctx, Http3Connection &conn) noexcept = nullptr;
    void (*on_connection_closed)(void *ctx, Http3Connection &conn) noexcept = nullptr;
    void (*on_stream_opened)(void *ctx, Http3Stream &stream) noexcept = nullptr;
    void (*on_stream_readable)(void *ctx, Http3Stream &stream) noexcept = nullptr;
    void (*on_stream_writable)(void *ctx, Http3Stream &stream) noexcept = nullptr;
    void (*on_stream_closed)(void *ctx, Http3Stream &stream) noexcept = nullptr;
};

struct Http3EngineHandler {
    const Http3EngineHandlerOps *ops = nullptr;
    void *ctx = nullptr;
};

struct Http3EngineConfig {
    Http3EngineOptions options{};
    TlsIdentitySelectorOps identity_selector{};
    Http3EngineHandler handler{};
};

class Http3Engine : public common::NonCopyable, public common::NonMovable {
public:
    Http3Engine(event::EventLoop &loop, net::UdpSocket &udp, Http3EngineConfig config) noexcept;
    ~Http3Engine();

    common::IoResult<void> init() noexcept;
    async::DetachedTask run() noexcept;
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_ENGINE_H
