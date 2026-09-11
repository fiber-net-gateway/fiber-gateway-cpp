#ifndef FIBER_HTTP_HTTP3_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP3_CLIENT_CONNECTION_H

#include <utility>

#include "../async/Task.h"
#include "../common/NonCopyable.h"
#include "../common/mem/BufPool.h"
#include "../quic/QuicConnection.h"
#include "Http3Protocol.h"

namespace fiber::http {

class ClientHttp3Exchange;
class Http3ClientConnectionImpl;

class Http3ClientConnection : public common::NonCopyable {
public:
    Http3ClientConnection() noexcept = default;
    Http3ClientConnection(Http3ClientConnection &&other) noexcept;
    Http3ClientConnection &operator=(Http3ClientConnection &&other) noexcept;
    ~Http3ClientConnection();

    [[nodiscard]] ClientHttp3Exchange open_exchange(mem::BufPool &pool) noexcept;

    void shutdown(Http3ErrorCode error = Http3ErrorCode::RequestCancelled) noexcept;
    void graceful_shutdown(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    // After shutdown/drain or peer closure, join H3 tasks. Does not wait for
    // QUIC endpoint detach or release this handle. The handle must be valid
    // until the lazy task starts; the running wait retains the connection.
    async::Task<void> wait_closed() noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr && static_cast<bool>(quic_); }
    [[nodiscard]] bool accepting_requests() const noexcept;
    [[nodiscard]] Http3ConnectionState state() const noexcept;
    [[nodiscard]] Http3ErrorCode close_error() const noexcept;
    [[nodiscard]] bool peer_settings_received() const noexcept;
    [[nodiscard]] const Http3Settings &local_settings() const noexcept;
    [[nodiscard]] const Http3Settings &peer_settings() const noexcept;
    [[nodiscard]] bool peer_goaway_received() const noexcept;
    [[nodiscard]] std::uint64_t peer_goaway_id() const noexcept;
    [[nodiscard]] quic::QuicConnection &quic() noexcept;
    [[nodiscard]] const quic::QuicConnection &quic() const noexcept;

private:
    Http3ClientConnection(quic::QuicConnection::Lease quic, Http3ClientConnectionImpl &impl) noexcept :
        quic_(std::move(quic)), impl_(&impl) {}

    quic::QuicConnection::Lease quic_{};
    Http3ClientConnectionImpl *impl_ = nullptr;

    friend class Http3Client;
    friend class Http3ClientConnectionImpl;
    friend class ClientHttp3Exchange;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CLIENT_CONNECTION_H
