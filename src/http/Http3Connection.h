#ifndef FIBER_HTTP_HTTP3_CONNECTION_H
#define FIBER_HTTP_HTTP3_CONNECTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../quic/QuicConnection.h"
#include "Http3Protocol.h"

namespace fiber::http {

enum class Http3ConnectionState : std::uint8_t {
    Init,
    Running,
    Draining,
    Closing,
    Closed,
};

class Http3Connection : public common::NonCopyable, public common::NonMovable {
public:
    struct Ops {
        quic::QuicStream::Lease (*create_server_request)(void *owner, std::uint64_t stream_id,
                                                         Http3Connection &conn) noexcept = nullptr;
    };

    struct Options {
        Http3Settings local_settings{};
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(3);
        void *owner = nullptr;
        Ops ops{};
        bool enable_push = false;
    };

    explicit Http3Connection(quic::QuicConnection &quic) noexcept;
    Http3Connection(quic::QuicConnection &quic, const Options &options) noexcept;
    ~Http3Connection();

    [[nodiscard]] quic::QuicConnection &quic() noexcept { return quic_; }
    [[nodiscard]] const quic::QuicConnection &quic() const noexcept { return quic_; }
    [[nodiscard]] quic::QuicConnectionRole role() const noexcept { return quic_.role(); }
    [[nodiscard]] Http3ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] Http3ErrorCode close_error() const noexcept { return close_error_; }
    [[nodiscard]] const Http3Settings &local_settings() const noexcept { return options_.local_settings; }
    [[nodiscard]] const Http3Settings &peer_settings() const noexcept { return peer_settings_; }
    [[nodiscard]] bool peer_settings_received() const noexcept { return peer_settings_received_; }
    [[nodiscard]] bool peer_control_stream_seen() const noexcept { return peer_control_seen_; }
    [[nodiscard]] bool peer_qpack_encoder_stream_seen() const noexcept { return peer_qpack_encoder_seen_; }
    [[nodiscard]] bool peer_qpack_decoder_stream_seen() const noexcept { return peer_qpack_decoder_seen_; }
    [[nodiscard]] bool closing() const noexcept {
        return state_ == Http3ConnectionState::Draining || state_ == Http3ConnectionState::Closing ||
               state_ == Http3ConnectionState::Closed;
    }

    async::Task<common::IoResult<void>> start() noexcept;
    common::IoResult<void> apply_peer_settings(const Http3Settings &settings) noexcept;
    void graceful_shutdown(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    void close(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    void mark_closed() noexcept;
    async::Task<void> wait_closed() noexcept;

private:
    struct PeerStreamReader {
        quic::QuicStream::Lease lease{};
        Http3StreamKind kind = Http3StreamKind::Unclassified;
        common::IntrusiveListHook link{};
    };

    using PeerStreamReaderList = common::IntrusiveList<PeerStreamReader, offsetof(PeerStreamReader, link)>;

    [[nodiscard]] static const quic::QuicConnection::Ops &quic_ops() noexcept;
    [[nodiscard]] static quic::QuicStream::Lease create_owned_stream() noexcept;
    [[nodiscard]] static quic::QuicStream::Lease create_peer_stream(void *owner, std::uint64_t stream_id) noexcept;
    static void on_peer_stream_attached(void *owner, quic::QuicStream &stream) noexcept;
    static void destroy_peer_stream(void *owner, quic::QuicStream &stream) noexcept;

    void handle_peer_stream_attached(quic::QuicStream &stream) noexcept;
    void start_peer_uni_reader(quic::QuicStream &stream) noexcept;
    async::DetachedTask run_peer_uni_stream(quic::QuicStream::Lease stream) noexcept;
    [[nodiscard]] common::IoResult<void> register_peer_stream(PeerStreamReader &reader,
                                                              std::uint64_t stream_type) noexcept;
    void unregister_peer_stream(PeerStreamReader &reader) noexcept;
    void stop_peer_readers(Http3ErrorCode error) noexcept;
    void close_from_reader(Http3ErrorCode error) noexcept;

    quic::QuicConnection &quic_;
    Options options_{};
    Http3Settings peer_settings_{};
    quic::QuicStream::Lease local_control_stream_{};
    quic::QuicStream *peer_control_stream_ = nullptr;
    quic::QuicStream *peer_qpack_encoder_stream_ = nullptr;
    quic::QuicStream *peer_qpack_decoder_stream_ = nullptr;
    PeerStreamReaderList peer_readers_{};
    async::WaitGroup peer_reader_group_{};
    Http3ConnectionState state_ = Http3ConnectionState::Init;
    Http3ErrorCode close_error_ = Http3ErrorCode::NoError;
    bool peer_settings_received_ = false;
    bool peer_control_seen_ = false;
    bool peer_qpack_encoder_seen_ = false;
    bool peer_qpack_decoder_seen_ = false;
    bool stopping_readers_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CONNECTION_H
