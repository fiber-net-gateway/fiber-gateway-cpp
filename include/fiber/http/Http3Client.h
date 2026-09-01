#ifndef FIBER_HTTP_HTTP3_CLIENT_H
#define FIBER_HTTP_HTTP3_CLIENT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/TlsContext.h"
#include "../quic/QuicClient.h"
#include "../quic/QuicUdpEndpoint.h"
#include "Http3ClientConnection.h"
#include "Http3Connection.h"

namespace fiber::http {

enum class Http3ClientConnectPhase : std::uint8_t {
    ClientInit,
    Quic,
    Alpn,
    Http3,
};

struct Http3ClientConnectError {
    Http3ClientConnectPhase phase = Http3ClientConnectPhase::ClientInit;
    common::IoErr io_error = common::IoErr::Unknown;
    quic::QuicConnectError quic_error{};
};

using Http3ClientConnectResult = std::expected<Http3ClientConnection, Http3ClientConnectError>;

class Http3Client : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::TlsClientConnectionOptions tls{.verify_peer = true};
        quic::QuicClientCacheOps cache{};
        Http3Settings local_settings{};
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(3);
        std::uint32_t max_qpack_string_size = 64 * 1024;
        std::size_t max_field_section_size = 128 * 1024;
    };

    Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept;
    // Cache callbacks and connection factories retain this object as their owner.
    // The client and endpoint must outlive every connection created by connect().
    ~Http3Client() = default;

    [[nodiscard]] common::IoResult<void> init() noexcept;
    [[nodiscard]] async::Task<Http3ClientConnectResult> connect(quic::QuicClientConnectOptions options) noexcept;

    [[nodiscard]] const net::TlsContext *tls_context() const noexcept { return options_.tls.context; }

private:
    class Session;

    [[nodiscard]] static net::TlsClientConnectionOptions
    normalize_tls_options(net::TlsClientConnectionOptions options) noexcept;
    [[nodiscard]] static quic::QuicConnection::Lease
    create_connection_op(void *owner, const quic::QuicConnection::Options &options) noexcept;
    [[nodiscard]] quic::QuicConnection::Lease create_connection(const quic::QuicConnection::Options &options) noexcept;
    [[nodiscard]] Http3Connection::Options make_h3_options() const noexcept;
    [[nodiscard]] static Http3ClientConnectError make_error(Http3ClientConnectPhase phase,
                                                            common::IoErr error) noexcept;

    quic::QuicUdpEndpoint *endpoint_ = nullptr;
    Options options_{};
    quic::QuicClient quic_client_{};
    Session *last_created_session_ = nullptr;
    bool initialized_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CLIENT_H
