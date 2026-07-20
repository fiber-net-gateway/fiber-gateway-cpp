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
class Http3Connection;

class Http3ClientConnection : public common::NonCopyable {
public:
    Http3ClientConnection() noexcept = default;
    Http3ClientConnection(Http3ClientConnection &&other) noexcept;
    Http3ClientConnection &operator=(Http3ClientConnection &&other) noexcept;
    ~Http3ClientConnection();

    [[nodiscard]] ClientHttp3Exchange open_exchange(mem::BufPool &pool) noexcept;

    void shutdown(Http3ErrorCode error = Http3ErrorCode::RequestCancelled) noexcept;
    void graceful_shutdown(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    async::Task<void> wait_closed() noexcept;

    [[nodiscard]] bool valid() const noexcept { return h3_ != nullptr && static_cast<bool>(quic_); }
    [[nodiscard]] Http3Connection &http3() noexcept;
    [[nodiscard]] const Http3Connection &http3() const noexcept;
    [[nodiscard]] quic::QuicConnection &quic() noexcept;
    [[nodiscard]] const quic::QuicConnection &quic() const noexcept;

private:
    Http3ClientConnection(quic::QuicConnection::Lease quic, Http3Connection &h3) noexcept :
        quic_(std::move(quic)), h3_(&h3) {}

    quic::QuicConnection::Lease quic_{};
    Http3Connection *h3_ = nullptr;

    friend class Http3Client;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CLIENT_CONNECTION_H
