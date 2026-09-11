#ifndef FIBER_HTTP_HTTP3_CONTROL_STREAMS_H
#define FIBER_HTTP_HTTP3_CONTROL_STREAMS_H
#include <fiber/async/Spawn.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/IntrusiveList.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/quic/QuicLocalStreamGate.h>
#include "http/Http3ControlStreamDecoder.h"
namespace fiber::http {
// Protocol streams only. The owner controls admission, requests and shutdown.
class Http3ControlStreams : public common::NonCopyable, public common::NonMovable {
public:
    struct Ops {
        Http3ErrorCode (*on_control_event)(void *, const Http3ControlStreamEvent &) noexcept;
        Http3ErrorCode (*on_push_stream)(void *) noexcept;
        void (*on_error)(void *, Http3ErrorCode) noexcept;
    };
    Http3ControlStreams(quic::QuicConnection &quic, quic::QuicLocalStreamGate &gate, Http3Settings settings,
                        void *owner, const Ops &ops) noexcept;
    ~Http3ControlStreams();
    static quic::QuicStream::Lease create_stream() noexcept;
    async::Task<common::IoResult<void>> start() noexcept;
    void accept_peer_stream(quic::QuicStream &stream) noexcept;
    async::Task<common::IoResult<void>> send_goaway(std::uint64_t id) noexcept;
    void stop(Http3ErrorCode error) noexcept;
    async::Task<void> join_readers() noexcept;
    common::IoResult<void> apply_peer_settings(const Http3Settings &settings) noexcept;
    const Http3Settings &local_settings() const noexcept { return local_settings_; }
    const Http3Settings &peer_settings() const noexcept { return peer_settings_; }
    bool peer_settings_received() const noexcept { return peer_settings_received_; }
    bool local_control_stream_available() const noexcept { return static_cast<bool>(local_control_stream_); }
    bool peer_control_stream_seen() const noexcept { return peer_control_seen_; }
    bool peer_qpack_encoder_stream_seen() const noexcept { return peer_qpack_encoder_seen_; }
    bool peer_qpack_decoder_stream_seen() const noexcept { return peer_qpack_decoder_seen_; }

private:
    struct PeerStreamReader {
        quic::QuicStream::Lease lease{};
        Http3StreamKind kind = Http3StreamKind::Unclassified;
        common::IntrusiveListHook link{};
    };
    using PeerStreamReaderList = common::IntrusiveList<PeerStreamReader, offsetof(PeerStreamReader, link)>;
    static void destroy_stream(void *, quic::QuicStream &stream) noexcept;
    async::DetachedTask run_peer_uni_stream(quic::QuicStream::Lease stream) noexcept;
    common::IoResult<void> register_peer_stream(PeerStreamReader &reader, std::uint64_t type) noexcept;
    void unregister_peer_stream(PeerStreamReader &reader) noexcept;
    void fail(Http3ErrorCode error) noexcept;
    quic::QuicConnection &quic_;
    quic::QuicLocalStreamGate &local_stream_gate_;
    const Http3Settings local_settings_;
    void *owner_;
    Ops ops_;
    Http3Settings peer_settings_{};
    quic::QuicStream::Lease local_control_stream_{};
    PeerStreamReaderList peer_readers_{};
    async::WaitGroup peer_reader_group_{};
    bool peer_settings_received_ = false;
    bool peer_control_seen_ = false;
    bool peer_qpack_encoder_seen_ = false;
    bool peer_qpack_decoder_seen_ = false;
    bool stopping_readers_ = false;
};
} // namespace fiber::http
#endif
