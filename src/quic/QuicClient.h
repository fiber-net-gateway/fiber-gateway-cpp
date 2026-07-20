#ifndef FIBER_QUIC_QUIC_CLIENT_H
#define FIBER_QUIC_QUIC_CLIENT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/SocketAddress.h"
#include "QuicConnection.h"

struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace fiber::net {
class TlsContext;
}

namespace fiber::quic {

class QuicUdpEndpoint;

struct QuicClientCacheKey {
    std::string_view server_name{};
    net::SocketAddress remote_addr{};
    const net::TlsContext *tls_context = nullptr;
};

struct QuicClientCachedState {
    SSL_SESSION *session = nullptr;
    const std::uint8_t *token = nullptr;
    std::size_t token_len = 0;
    QuicTransportSettings remembered_transport{};
    bool has_remembered_transport = false;
};

struct QuicClientCacheOps {
    void *owner = nullptr;
    bool (*load)(void *owner, const QuicClientCacheKey &key, QuicClientCachedState &out) noexcept = nullptr;
    // Returning true transfers the callback's SSL_SESSION reference to the cache.
    bool (*store_session)(void *owner, const QuicClientCacheKey &key, SSL_SESSION *session,
                          const QuicTransportSettings &remembered_transport) noexcept = nullptr;
    void (*store_token)(void *owner, const QuicClientCacheKey &key, const std::uint8_t *token,
                        std::size_t token_len) noexcept = nullptr;
};

enum class QuicConnectPhase : std::uint8_t {
    Endpoint,
    Connection,
    InitialCrypto,
    Tls,
    Handshake,
    VersionNegotiation,
    TransportParameters,
    Timeout,
    PeerClose,
};

struct QuicConnectError {
    QuicConnectPhase phase = QuicConnectPhase::Connection;
    common::IoErr io_error = common::IoErr::Unknown;
    QuicCloseInfo close{};
    long tls_verify_result = 0;
    std::uint8_t tls_alert = 0;
    std::uint32_t offered_version = 0;
};

using QuicConnectWaitResult = std::expected<void, QuicConnectError>;
using QuicConnectResult = std::expected<QuicConnection::Lease, QuicConnectError>;

struct QuicClientConnectOptions {
    net::SocketAddress remote_addr{};
    std::string server_name{};
    QuicTransportSettings transport{};
    QuicRecvFlowControlSettings recv_flow{};
    std::chrono::milliseconds keepalive_interval{0};
    std::chrono::milliseconds handshake_timeout{10000};
    std::uint64_t max_peer_bidirectional_streams = kQuicDefaultMaxBidirectionalStreams;
    std::uint64_t max_peer_unidirectional_streams = kQuicDefaultMaxUnidirectionalStreams;
    void *application_owner = nullptr;
    QuicConnection::Ops application_ops{};
    bool allow_insecure = false;
    bool enable_early_data = false;
};

class QuicClientAttempt : public common::NonCopyable {
public:
    QuicClientAttempt() noexcept = default;
    QuicClientAttempt(QuicClientAttempt &&other) noexcept;
    QuicClientAttempt &operator=(QuicClientAttempt &&other) noexcept;
    ~QuicClientAttempt();

    [[nodiscard]] QuicConnection *connection() noexcept { return connection_.get(); }
    [[nodiscard]] const QuicConnection *connection() const noexcept { return connection_.get(); }
    [[nodiscard]] bool valid() const noexcept { return connection_.get() != nullptr; }

    [[nodiscard]] async::Task<QuicConnectWaitResult>
    wait_connected(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] async::Task<QuicConnectWaitResult>
    wait_confirmed(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    void cancel() noexcept;
    [[nodiscard]] QuicConnection::Lease release() noexcept { return std::move(connection_); }

private:
    explicit QuicClientAttempt(QuicConnection::Lease connection,
                               std::chrono::steady_clock::time_point handshake_deadline) noexcept :
        connection_(std::move(connection)), handshake_deadline_(handshake_deadline) {}
    [[nodiscard]] QuicConnectError make_error(common::IoErr error) const noexcept;

    QuicConnection::Lease connection_{};
    std::chrono::steady_clock::time_point handshake_deadline_{std::chrono::steady_clock::time_point::max()};

    friend class QuicClient;
};

class QuicClient : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        void *connection_owner = nullptr;
        QuicConnection::Lease (*create_connection)(void *owner,
                                                   const QuicConnection::Options &options) noexcept = nullptr;
        QuicClientCacheOps cache{};
    };

    QuicClient() noexcept = default;
    // The connector owns cache callback state referenced by its connections;
    // it must outlive every connection started through it.
    ~QuicClient() = default;

    [[nodiscard]] common::IoResult<void> init(QuicUdpEndpoint &endpoint, net::TlsContext &tls_context,
                                              const Options &options) noexcept;
    [[nodiscard]] std::expected<QuicClientAttempt, QuicConnectError>
    start_connect(const QuicClientConnectOptions &options) noexcept;
    [[nodiscard]] async::Task<QuicConnectResult> connect(const QuicClientConnectOptions &options) noexcept;

private:
    [[nodiscard]] static QuicConnectError error(QuicConnectPhase phase, common::IoErr io_error) noexcept;
    [[nodiscard]] static bool store_session(void *owner, QuicConnection &connection, SSL_SESSION *session) noexcept;
    static void store_token(void *owner, QuicConnection &connection, const std::uint8_t *token,
                            std::size_t token_len) noexcept;
    [[nodiscard]] QuicClientCacheKey cache_key(std::string_view server_name,
                                               const net::SocketAddress &remote_addr) const noexcept;

    QuicUdpEndpoint *endpoint_ = nullptr;
    net::TlsContext *tls_context_ = nullptr;
    Options options_{};
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CLIENT_H
