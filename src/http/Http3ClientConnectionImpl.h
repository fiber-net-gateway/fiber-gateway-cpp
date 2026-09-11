#ifndef FIBER_HTTP_HTTP3_CLIENT_CONNECTION_IMPL_H
#define FIBER_HTTP_HTTP3_CLIENT_CONNECTION_IMPL_H
#include <fiber/http/Http3ClientConnection.h>
#include "http/Http3ControlStreams.h"
namespace fiber::http {
struct Http3ClientRequestEntry {
    void *owner = nullptr;
    void (*on_rejected)(void *, std::uint64_t) noexcept = nullptr;
    void (*on_connection_close)(void *, Http3ErrorCode) noexcept = nullptr;
    std::uint64_t stream_id = quic::kQuicUnassignedStreamId;
    common::IntrusiveListHook link{};
};
// Stable storage shared by the movable handle and requests. QUIC leases own it.
class Http3ClientConnectionImpl : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        Http3Settings local_settings{};
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(3);
        std::uint32_t max_qpack_string_size = 64 * 1024;
        std::size_t max_field_section_size = 128 * 1024;
    };
    static Http3ClientConnectionImpl *create(const quic::QuicConnection::Options &, const Options &) noexcept;
    Http3ClientConnection make_handle(quic::QuicConnection::Lease lease) noexcept;
    quic::QuicConnection &quic() noexcept { return quic_; }
    const quic::QuicConnection &quic() const noexcept { return quic_; }
    quic::QuicLocalStreamGate &local_stream_gate() noexcept { return local_stream_gate_; }
    const Http3Settings &local_settings() const noexcept { return control_.local_settings(); }
    const Http3Settings &peer_settings() const noexcept { return control_.peer_settings(); }
    bool peer_settings_received() const noexcept { return control_.peer_settings_received(); }
    std::uint32_t max_qpack_string_size() const noexcept { return max_qpack_string_size_; }
    std::size_t max_field_section_size() const noexcept { return max_field_section_size_; }
    Http3ConnectionState state() const noexcept { return state_; }
    Http3ErrorCode close_error() const noexcept { return close_error_; }
    bool peer_goaway_received() const noexcept { return peer_goaway_received_; }
    std::uint64_t peer_goaway_id() const noexcept { return peer_goaway_id_; }
    bool accepting_requests() const noexcept {
        return state_ == Http3ConnectionState::Running && !peer_goaway_received_ && quic_.accepting_new_streams();
    }
    async::Task<common::IoResult<void>> start() noexcept;
    common::IoResult<void> register_client_request(Http3ClientRequestEntry &) noexcept;
    void unregister_client_request(Http3ClientRequestEntry &) noexcept;
    void graceful_shutdown(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    void close(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    async::Task<void> wait_closed() noexcept;

private:
    Http3ClientConnectionImpl(const quic::QuicConnection::Options &, const Options &) noexcept;
    ~Http3ClientConnectionImpl();
    static quic::QuicConnection::Options make_quic_options(const quic::QuicConnection::Options &,
                                                           Http3ClientConnectionImpl *) noexcept;
    static quic::QuicStream::Lease create_peer_stream(void *, std::uint64_t) noexcept;
    static void on_peer_stream_attached(void *, quic::QuicStream &) noexcept;
    static void on_quic_state_change(void *, quic::QuicConnection &) noexcept;
    static void on_quic_capacity_change(void *, quic::QuicConnection &) noexcept;
    static const Http3ControlStreams::Ops &control_ops() noexcept;
    static void destroy_connection(void *, quic::QuicConnection &) noexcept;
    async::Task<void> join_protocol_tasks() noexcept;
    async::DetachedTask run_graceful_shutdown(Http3ErrorCode) noexcept;
    common::IoResult<void> apply_peer_goaway(std::uint64_t) noexcept;
    void reject_client_requests(std::uint64_t) noexcept;
    void detach_client_requests(Http3ErrorCode) noexcept;
    using ClientRequestList = common::IntrusiveList<Http3ClientRequestEntry, offsetof(Http3ClientRequestEntry, link)>;
    quic::QuicConnection quic_;
    quic::QuicLocalStreamGate local_stream_gate_;
    Http3ControlStreams control_;
    const std::chrono::milliseconds drain_timeout_;
    const std::uint32_t max_qpack_string_size_;
    const std::size_t max_field_section_size_;
    ClientRequestList client_requests_{};
    async::WaitGroup client_request_group_{};
    async::WaitGroup start_tasks_{};
    async::WaitGroup drain_tasks_{};
    Http3ConnectionState state_ = Http3ConnectionState::Prepared;
    Http3ErrorCode close_error_ = Http3ErrorCode::NoError;
    std::uint64_t peer_goaway_id_ = 0;
    bool peer_goaway_received_ = false;
    bool cleanup_started_ = false;
};
} // namespace fiber::http
#endif
