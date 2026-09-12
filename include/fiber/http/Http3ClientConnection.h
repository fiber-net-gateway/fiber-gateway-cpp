#ifndef FIBER_HTTP_HTTP3_CLIENT_CONNECTION_H
#define FIBER_HTTP_HTTP3_CLIENT_CONNECTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "../async/LocalWaitGroup.h"
#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../net/SocketAddress.h"
#include "../net/TlsParams.h"
#include "../quic/QuicClientConnect.h"
#include "../quic/QuicConnection.h"
#include "../quic/QuicLocalStreamGate.h"
#include "Http3ControlStreams.h"
#include "Http3Protocol.h"

namespace fiber::http {

class ClientHttp3Exchange;
class ClientHttp3Request;
class Http3Client;
struct Http3ClientConnectionTestAccess;

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

using Http3ClientConnectResult = std::expected<void, Http3ClientConnectError>;

struct Http3ClientConnectOptions {
    net::SocketAddress remote_addr{};
    // Borrowed until the Http3ClientConnection constructor returns; the
    // connection keeps its own copies for the session cache key.
    std::string_view server_name{};
    std::string_view verify_name{};
    quic::QuicTransportSettings transport{};
    quic::QuicRecvFlowControlSettings recv_flow{};
    std::chrono::milliseconds keepalive_interval{0};
    std::chrono::milliseconds handshake_timeout{net::kDefaultTlsHandshakeTimeout};
    bool allow_insecure = false;
};

// A request's registration with its connection, so GOAWAY and connection
// close reach it. Internal to ClientHttp3Request; public only because the
// connection embeds the list.
struct Http3ClientRequestEntry {
    void *owner = nullptr;
    void (*on_rejected)(void *, std::uint64_t) noexcept = nullptr;
    void (*on_connection_close)(void *, Http3ErrorCode) noexcept = nullptr;
    std::uint64_t stream_id = quic::kQuicUnassignedStreamId;
    common::IntrusiveListHook link{};
};

// One HTTP/3 client connection, owned by the caller like Http2ClientConnection:
// construct on the endpoint's loop, co_await connect(), open exchanges,
// shutdown()/graceful_shutdown(), co_await wait_closed(), destroy. The QUIC
// connection is embedded and caller-owned (no lease-driven release): the
// endpoint hosts it until it detaches and must itself outlive this object.
class Http3ClientConnection : public common::NonCopyable, public common::NonMovable {
public:
    // On the endpoint's loop (asserted). Allocates the QUIC identity from the
    // endpoint and builds the QUIC connection in place; nothing is registered
    // or sent until connect(). Never fails: an identity allocation failure
    // (endpoint not initialized) is reported by connect() as phase Quic /
    // QuicConnectPhase::Connection.
    Http3ClientConnection(Http3Client &client, const Http3ClientConnectOptions &options) noexcept;
    // Never connected, or closed and detached from the endpoint, with no
    // exchange, request or wait_closed() joiner alive. Asserted.
    ~Http3ClientConnection();

    // Exactly once, on the endpoint's loop (asserted). Loads the session
    // cache, runs QUIC connect, waits for the handshake (handshake_timeout),
    // verifies ALPN "h3" and starts the HTTP/3 control streams. Every failure
    // returns with this object Closed and destructible: one after the
    // connection attached closes it immediately and waits for detach first.
    // Canceling the task after attach also begins immediate closure, but
    // cannot await cleanup: the caller must co_await wait_closed() before
    // destroying this object.
    [[nodiscard]] async::Task<Http3ClientConnectResult> connect() noexcept;

    // The buffer pool and this connection must outlive the exchange.
    [[nodiscard]] ClientHttp3Exchange open_exchange(mem::BufPool &pool) noexcept;

    void shutdown(Http3ErrorCode error = Http3ErrorCode::RequestCancelled) noexcept;
    void graceful_shutdown(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    // Joins the H3 start/reader/drain tasks and then waits for the QUIC
    // connection to detach from the endpoint. After it returns this object
    // may be destroyed. Repeatable.
    [[nodiscard]] async::Task<void> wait_closed() noexcept;

    [[nodiscard]] bool accepting_requests() const noexcept {
        return state_ == Http3ConnectionState::Running && !peer_goaway_received_ && quic_.accepting_new_streams();
    }
    [[nodiscard]] Http3ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] Http3ErrorCode close_error() const noexcept { return close_error_; }
    [[nodiscard]] bool peer_settings_received() const noexcept { return control_.peer_settings_received(); }
    [[nodiscard]] const Http3Settings &local_settings() const noexcept { return control_.local_settings(); }
    [[nodiscard]] const Http3Settings &peer_settings() const noexcept { return control_.peer_settings(); }
    [[nodiscard]] bool peer_goaway_received() const noexcept { return peer_goaway_received_; }
    [[nodiscard]] std::uint64_t peer_goaway_id() const noexcept { return peer_goaway_id_; }
    [[nodiscard]] quic::QuicConnection &quic() noexcept { return quic_; }
    [[nodiscard]] const quic::QuicConnection &quic() const noexcept { return quic_; }
    // FIFO admission for locally initiated streams on this connection.
    [[nodiscard]] quic::QuicLocalStreamGate &local_stream_gate() noexcept { return local_stream_gate_; }

private:
    using ClientRequestList = common::IntrusiveList<Http3ClientRequestEntry, offsetof(Http3ClientRequestEntry, link)>;

    // Test seam: builds on caller-provided QUIC options instead of an
    // endpoint-allocated identity, leaving connect() out of the picture.
    Http3ClientConnection(Http3Client &client, const quic::QuicConnection::Options &quic_options,
                          const Http3ClientConnectOptions &options) noexcept;
    [[nodiscard]] static quic::QuicConnection::Options make_quic_options(Http3Client &client,
                                                                         const Http3ClientConnectOptions &options,
                                                                         common::IoErr &identity_error) noexcept;
    [[nodiscard]] static quic::QuicConnection::Options own_quic_options(const quic::QuicConnection::Options &base,
                                                                        Http3ClientConnection *owner) noexcept;
    static quic::QuicStream::Lease create_peer_stream(void *, std::uint64_t) noexcept;
    static void on_peer_stream_attached(void *, quic::QuicStream &) noexcept;
    static void on_quic_state_change(void *, quic::QuicConnection &) noexcept;
    static void on_quic_capacity_change(void *, quic::QuicConnection &) noexcept;
    static bool on_new_tls_session(void *, quic::QuicConnection &, SSL_SESSION *) noexcept;
    static void on_new_token(void *, quic::QuicConnection &, const std::uint8_t *, std::size_t) noexcept;
    [[nodiscard]] static const Http3ControlStreams::Ops &control_ops() noexcept;
    [[nodiscard]] quic::QuicClientCacheKey cache_key() const noexcept;
    [[nodiscard]] std::uint32_t max_qpack_string_size() const noexcept { return max_qpack_string_size_; }
    [[nodiscard]] std::size_t max_field_section_size() const noexcept { return max_field_section_size_; }
    // Opens the local control stream and sends SETTINGS; connect() runs it
    // once the QUIC handshake is established.
    [[nodiscard]] async::Task<common::IoResult<void>> start() noexcept;
    // Immediate close; connect()'s failure paths run it after an immediate
    // QUIC close so no closing period is spent.
    void close(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    // Shared by failed and canceled connect attempts: an immediate QUIC close
    // (which also cuts a Closing or Draining period short), or Closed in place
    // for a connection that never attached, then the HTTP/3 close. Cleanup
    // completes through wait_closed().
    void abort_connect(Http3ErrorCode close_code) noexcept;
    [[nodiscard]] async::Task<Http3ClientConnectResult> fail_connect(Http3ClientConnectError error,
                                                                     Http3ErrorCode close_code) noexcept;
    [[nodiscard]] common::IoResult<void> register_client_request(Http3ClientRequestEntry &) noexcept;
    void unregister_client_request(Http3ClientRequestEntry &) noexcept;
    [[nodiscard]] async::Task<void> join_protocol_tasks() noexcept;
    async::DetachedTask run_graceful_shutdown(Http3ErrorCode) noexcept;
    [[nodiscard]] common::IoResult<void> apply_peer_goaway(std::uint64_t) noexcept;
    void reject_client_requests(std::uint64_t) noexcept;
    void detach_client_requests(Http3ErrorCode) noexcept;

    Http3Client &client_;
    // Session cache identity. The address is the dial target, not the
    // (migratable) QUIC path.
    const std::string server_name_;
    const std::string verify_name_;
    const net::SocketAddress remote_addr_;
    const std::chrono::milliseconds handshake_timeout_;
    const bool allow_insecure_;
    // Set by the constructor when the endpoint could not allocate an identity;
    // surfaced by connect().
    common::IoErr identity_error_ = common::IoErr::None;
    quic::QuicConnection quic_;
    quic::QuicLocalStreamGate local_stream_gate_;
    Http3ControlStreams control_;
    const std::chrono::milliseconds drain_timeout_;
    const std::uint32_t max_qpack_string_size_;
    const std::size_t max_field_section_size_;
    ClientRequestList client_requests_{};
    async::LocalWaitGroup client_request_group_{};
    async::LocalWaitGroup start_tasks_{};
    async::LocalWaitGroup drain_tasks_{};
    Http3ConnectionState state_ = Http3ConnectionState::Prepared;
    Http3ErrorCode close_error_ = Http3ErrorCode::NoError;
    std::uint64_t peer_goaway_id_ = 0;
    bool peer_goaway_received_ = false;

    friend class ClientHttp3Exchange;
    friend class ClientHttp3Request;
    friend struct Http3ClientConnectionTestAccess;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CLIENT_CONNECTION_H
