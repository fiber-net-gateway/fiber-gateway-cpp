#include "http/Http3ControlStreams.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http3Codec.h>
#include "http/Http3ControlStreamDecoder.h"
#include "http/Http3ControlStreamEncoder.h"
#include "http/Http3QpackControlStreamDecoder.h"

namespace fiber::http {

namespace {

constexpr std::size_t kHttp3ReadChunkSize = 4096;

[[nodiscard]] std::uint64_t error_value(Http3ErrorCode error) noexcept { return static_cast<std::uint64_t>(error); }

[[nodiscard]] bool is_terminal_reader_error(common::IoErr err) noexcept {
    return err == common::IoErr::Canceled || err == common::IoErr::ConnReset || err == common::IoErr::BrokenPipe;
}

} // namespace

Http3ControlStreams::Http3ControlStreams(quic::QuicConnection &quic, quic::QuicLocalStreamGate &gate,
                                         Http3Settings settings, void *owner, const Ops &ops) noexcept :
    quic_(quic), local_stream_gate_(gate), local_settings_(settings), owner_(owner), ops_(ops) {
    FIBER_ASSERT(owner_ != nullptr && ops_.on_control_event != nullptr && ops_.on_push_stream != nullptr &&
                 ops_.on_error != nullptr);
}
Http3ControlStreams::~Http3ControlStreams() {
    FIBER_ASSERT(peer_reader_group_.empty());
    FIBER_ASSERT(peer_readers_.empty());
}
quic::QuicStream::Lease Http3ControlStreams::create_stream() noexcept {
    auto *stream = new (std::nothrow) quic::QuicStream(nullptr, &Http3ControlStreams::destroy_stream);
    return quic::QuicStream::Lease::adopt(stream);
}

async::Task<common::IoResult<void>> Http3ControlStreams::start() noexcept {
    // A closing transport reports Canceled without an H3 close so the owners can
    // let the QUIC teardown path drive the cleanup, matching the shared
    // connection start behavior this component replaced.
    if (stopping_readers_ || quic_.closing()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    FIBER_ASSERT(!local_control_stream_);
    auto preface = encode_http3_control_stream_preface(local_settings_, quic_.recv_extent_pool());
    if (!preface) {
        co_return std::unexpected(preface.error());
    }


    quic::QuicStream::Lease control_stream = create_stream();
    if (!control_stream) {
        fail(Http3ErrorCode::InternalError);
        co_return std::unexpected(common::IoErr::NoMem);
    }

    auto attached = co_await local_stream_gate_.attach(std::move(control_stream), quic::QuicStreamType::Unidirectional);
    if (!attached) {
        fail(Http3ErrorCode::StreamCreationError);
        co_return std::unexpected(attached.error());
    }

    local_control_stream_ = (*attached)->lease();
    mem::IoBufChain control_preface = std::move(*preface);
    while (!control_preface.empty()) {
        auto written = co_await local_control_stream_->write(control_preface);
        if (!written) {
            fail(Http3ErrorCode::ClosedCriticalStream);
            co_return std::unexpected(written.error());
        }
        if (*written == 0 && !control_preface.empty()) {
            fail(Http3ErrorCode::ClosedCriticalStream);
            co_return std::unexpected(common::IoErr::WouldBlock);
        }
    }

    co_return common::IoResult<void>{};
}

common::IoResult<void> Http3ControlStreams::apply_peer_settings(const Http3Settings &settings) noexcept {
    if (stopping_readers_) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (peer_settings_received_) {
        return std::unexpected(common::IoErr::Already);
    }
    peer_settings_ = settings;
    peer_settings_received_ = true;
    return {};
}

void Http3ControlStreams::accept_peer_stream(quic::QuicStream &stream) noexcept {
    FIBER_ASSERT(!stream.bidirectional());
    if (stopping_readers_) {
        stream.close(error_value(Http3ErrorCode::RequestCancelled));
        return;
    }
    quic::QuicStream::Lease lease = stream.lease();
    peer_reader_group_.add();
    async::spawn(quic_.loop(),
                 [this, lease = std::move(lease)]() mutable { return run_peer_uni_stream(std::move(lease)); });
}

async::DetachedTask Http3ControlStreams::run_peer_uni_stream(quic::QuicStream::Lease stream) noexcept {
    if (!stream || stopping_readers_) {
        peer_reader_group_.done();
        co_return;
    }

    struct ReaderScope {
        Http3ControlStreams &conn;
        PeerStreamReader &reader;

        ~ReaderScope() {
            conn.unregister_peer_stream(reader);
            conn.peer_reader_group_.done();
        }
    };

    PeerStreamReader reader{
            .lease = std::move(stream),
            .kind = Http3StreamKind::Unclassified,
            .link = {},
    };
    peer_readers_.push_back(reader);
    ReaderScope scope{*this, reader};
    mem::IoBufChain input;

    Http3VarintParser stream_type_parser;
    for (;;) {
        Http3ParseStatus status = stream_type_parser.parse(input);
        if (status == Http3ParseStatus::Done) {
            break;
        }
        if (status == Http3ParseStatus::Error) {
            fail(Http3ErrorCode::StreamCreationError);
            co_return;
        }

        auto read = co_await reader.lease->read(kHttp3ReadChunkSize, input);
        if (!read) {
            if (read.error() != common::IoErr::Canceled || !stopping_readers_) {
                fail(Http3ErrorCode::StreamCreationError);
            }
            co_return;
        }
        if (*read == 0 && input.complete()) {
            fail(Http3ErrorCode::StreamCreationError);
            co_return;
        }
    }

    if (stream_type_parser.value() == static_cast<std::uint64_t>(Http3StreamType::Push)) {
        auto accepted = ops_.on_push_stream(owner_);
        if (accepted != Http3ErrorCode::NoError) {
            fail(accepted);
        }
        co_return;
    }

    auto registered = register_peer_stream(reader, stream_type_parser.value());
    if (!registered) {
        if (registered.error() != common::IoErr::NotSupported) {
            fail(Http3ErrorCode::StreamCreationError);
            co_return;
        }
        (void) reader.lease->stop_read(error_value(Http3ErrorCode::NoError));
        co_return;
    }

    switch (reader.kind) {
        case Http3StreamKind::Control: {
            Http3ControlStreamDecoder decoder;
            for (;;) {
                Http3ControlStreamEvent event;
                Http3ParseStatus status = decoder.parse(input, event);
                if (status == Http3ParseStatus::Error) {
                    fail(decoder.error().h3_error);
                    co_return;
                }
                if (status == Http3ParseStatus::Done) {
                    if (event.type == Http3ControlStreamEventType::Settings) {
                        auto applied = apply_peer_settings(event.settings);
                        if (!applied) {
                            fail(Http3ErrorCode::SettingsError);
                            co_return;
                        }
                    } else {
                        auto error = ops_.on_control_event(owner_, event);
                        if (error != Http3ErrorCode::NoError) {
                            fail(error);
                            co_return;
                        }
                    }
                    continue;
                }

                input.drop_empty_front();
                auto read = co_await reader.lease->read(kHttp3ReadChunkSize, input);
                if (!read) {
                    if (read.error() == common::IoErr::Canceled && stopping_readers_) {
                        co_return;
                    }
                    fail(is_terminal_reader_error(read.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                : Http3ErrorCode::FrameError);
                    co_return;
                }
                if (*read == 0 && input.complete()) {
                    fail(Http3ErrorCode::ClosedCriticalStream);
                    co_return;
                }
            }
        }

        case Http3StreamKind::QpackEncoder: {
            Http3QpackEncoderStreamDecoder decoder;
            for (;;) {
                Http3ParseStatus status = decoder.parse(input);
                if (status == Http3ParseStatus::Error) {
                    fail(decoder.error().h3_error);
                    co_return;
                }

                input.drop_empty_front();
                auto read = co_await reader.lease->read(kHttp3ReadChunkSize, input);
                if (!read) {
                    if (read.error() == common::IoErr::Canceled && stopping_readers_) {
                        co_return;
                    }
                    fail(is_terminal_reader_error(read.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                : Http3ErrorCode::QpackEncoderStreamError);
                    co_return;
                }
                if (*read == 0 && input.complete()) {
                    fail(Http3ErrorCode::ClosedCriticalStream);
                    co_return;
                }
            }
        }

        case Http3StreamKind::QpackDecoder: {
            Http3QpackDecoderStreamDecoder decoder;
            for (;;) {
                Http3QpackDecoderStreamEvent event;
                Http3ParseStatus status = decoder.parse(input, event);
                if (status == Http3ParseStatus::Error) {
                    fail(decoder.error().h3_error);
                    co_return;
                }
                if (status == Http3ParseStatus::Done) {
                    continue;
                }

                input.drop_empty_front();
                auto read = co_await reader.lease->read(kHttp3ReadChunkSize, input);
                if (!read) {
                    if (read.error() == common::IoErr::Canceled && stopping_readers_) {
                        co_return;
                    }
                    fail(is_terminal_reader_error(read.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                : Http3ErrorCode::QpackDecoderStreamError);
                    co_return;
                }
                if (*read == 0 && input.complete()) {
                    fail(Http3ErrorCode::ClosedCriticalStream);
                    co_return;
                }
            }
        }

        default:
            co_return;
    }
}

common::IoResult<void> Http3ControlStreams::register_peer_stream(PeerStreamReader &reader,
                                                                 std::uint64_t stream_type) noexcept {
    switch (static_cast<Http3StreamType>(stream_type)) {
        case Http3StreamType::Control:
            if (peer_control_seen_) {
                return std::unexpected(common::IoErr::Already);
            }
            peer_control_seen_ = true;
            reader.kind = Http3StreamKind::Control;
            return {};
        case Http3StreamType::QpackEncoder:
            if (peer_qpack_encoder_seen_) {
                return std::unexpected(common::IoErr::Already);
            }
            peer_qpack_encoder_seen_ = true;
            reader.kind = Http3StreamKind::QpackEncoder;
            return {};
        case Http3StreamType::QpackDecoder:
            if (peer_qpack_decoder_seen_) {
                return std::unexpected(common::IoErr::Already);
            }
            peer_qpack_decoder_seen_ = true;
            reader.kind = Http3StreamKind::QpackDecoder;
            return {};
        default:
            reader.kind = Http3StreamKind::UnknownUni;
            return std::unexpected(common::IoErr::NotSupported);
    }
}

void Http3ControlStreams::unregister_peer_stream(PeerStreamReader &reader) noexcept { peer_readers_.erase(reader); }

void Http3ControlStreams::stop(Http3ErrorCode error) noexcept {
    stopping_readers_ = true;
    PeerStreamReader *reader = peer_readers_.front();
    while (reader != nullptr) {
        PeerStreamReader *next = peer_readers_.next_of(*reader);
        if (reader->lease) {
            (void) reader->lease->stop_read(error_value(error));
        }
        reader = next;
    }
}

void Http3ControlStreams::fail(Http3ErrorCode error) noexcept {
    if (!stopping_readers_) {
        ops_.on_error(owner_, error);
    }
}

async::Task<common::IoResult<void>> Http3ControlStreams::send_goaway(std::uint64_t id) noexcept {
    if (stopping_readers_) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    FIBER_ASSERT(local_control_stream_);
    auto frame = encode_http3_goaway_frame(id, quic_.recv_extent_pool());
    if (!frame) {
        fail(Http3ErrorCode::InternalError);
        co_return std::unexpected(frame.error());
    }
    while (!frame->empty()) {
        auto written = co_await local_control_stream_->write(*frame);
        if (!written || (*written == 0 && !frame->empty())) {
            fail(Http3ErrorCode::ClosedCriticalStream);
            co_return std::unexpected(written ? common::IoErr::WouldBlock : written.error());
        }
    }
    co_return common::IoResult<void>{};
}
async::Task<void> Http3ControlStreams::join_readers() noexcept { co_await peer_reader_group_.join(); }
void Http3ControlStreams::destroy_stream(void *, quic::QuicStream &stream) noexcept { delete &stream; }

} // namespace fiber::http
