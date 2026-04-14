#ifndef FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H

#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "TlsContext.h"

namespace fiber::http {

class ClientHttp1Exchange;
class HttpTransport;

struct Http1ClientConnectionOptions {
    net::SocketAddress peer_addr{};
    TlsOptions tls{};
    std::chrono::milliseconds connect_timeout{10000};
};

class Http1ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    Http1ClientConnection(event::EventLoop &loop, Http1ClientConnectionOptions options) noexcept;
    ~Http1ClientConnection();

    fiber::async::Task<common::IoResult<void>> connect() noexcept;
    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool reusable() const noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] const Http1ClientConnectionOptions &options() const noexcept { return options_; }

private:
    enum class State : std::uint8_t {
        Init,
        ConnectedIdle,
        Busy,
        Closed,
    };

    static Http1ClientConnectionOptions normalize_options(Http1ClientConnectionOptions options) noexcept;
    void mark_unusable() noexcept;
    [[nodiscard]] bool acquire_exchange(ClientHttp1Exchange *exchange) noexcept;
    void release_exchange(ClientHttp1Exchange *exchange, bool keepalive) noexcept;
    void fail_exchange(ClientHttp1Exchange *exchange) noexcept;

    friend class ClientHttp1Exchange;

    event::EventLoop *loop_ = nullptr;
    Http1ClientConnectionOptions options_{};
    TlsContext tls_ctx_;
    std::unique_ptr<HttpTransport> transport_;
    ClientHttp1Exchange *active_exchange_ = nullptr;
    State state_ = State::Init;
    bool keepalive_usable_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CLIENT_CONNECTION_H
