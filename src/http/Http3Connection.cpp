#include "Http3Connection.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>

#include "../common/Assert.h"
#include "../common/mem/IoBufChain.h"
#include "Http3Codec.h"
#include "Http3ControlStreamDecoder.h"
#include "Http3QpackControlStreamDecoder.h"

namespace fiber::http {

namespace {

constexpr std::size_t kHttp3ReadChunkSize = 4096;

[[nodiscard]] std::uint64_t error_value(Http3ErrorCode error) noexcept { return static_cast<std::uint64_t>(error); }

[[nodiscard]] bool is_terminal_reader_error(common::IoErr err) noexcept {
    return err == common::IoErr::Canceled || err == common::IoErr::ConnReset || err == common::IoErr::BrokenPipe;
}

} // namespace

Http3Connection::Http3Connection(quic::QuicConnection &quic) noexcept : Http3Connection(quic, Options{}) {}

Http3Connection::Http3Connection(quic::QuicConnection &quic, const Options &options) noexcept :
    quic_(quic), options_(options) {}

Http3Connection::~Http3Connection() {
    FIBER_ASSERT(peer_reader_group_.empty());
    FIBER_ASSERT(peer_readers_.empty());
}

const quic::QuicConnection::Ops &Http3Connection::quic_ops() noexcept {
    static const quic::QuicConnection::Ops kOps{
            .create_stream = &Http3Connection::create_peer_stream,
            .on_peer_stream_attached = &Http3Connection::on_peer_stream_attached,
    };
    return kOps;
}

quic::QuicStream::Lease Http3Connection::create_peer_stream(void *) noexcept {
    auto *stream = new (std::nothrow) quic::QuicStream(nullptr, &Http3Connection::destroy_peer_stream);
    return quic::QuicStream::Lease::adopt(stream);
}

void Http3Connection::destroy_peer_stream(void *, quic::QuicStream &stream) noexcept { delete &stream; }

void Http3Connection::on_peer_stream_attached(void *owner, quic::QuicStream &stream) noexcept {
    auto *conn = static_cast<Http3Connection *>(owner);
    FIBER_ASSERT(conn != nullptr);
    conn->handle_peer_stream_attached(stream);
}

common::IoResult<void> Http3Connection::start() noexcept {
    if (state_ != Http3ConnectionState::Init) {
        return std::unexpected(common::IoErr::Already);
    }
    if (quic_.closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    auto ops_set = quic_.set_app_ops(this, quic_ops());
    if (!ops_set) {
        return std::unexpected(ops_set.error());
    }
    state_ = Http3ConnectionState::Running;
    return {};
}

common::IoResult<void> Http3Connection::apply_peer_settings(const Http3Settings &settings) noexcept {
    if (closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (peer_settings_received_) {
        return std::unexpected(common::IoErr::Already);
    }
    peer_settings_ = settings;
    peer_settings_received_ = true;
    return {};
}

void Http3Connection::graceful_shutdown(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = Http3ConnectionState::Draining;
    stop_peer_readers(error);
}

void Http3Connection::close(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = Http3ConnectionState::Closing;
    stop_peer_readers(error);
    quic_.close_application(error_value(error));
}

void Http3Connection::mark_closed() noexcept { state_ = Http3ConnectionState::Closed; }

async::Task<void> Http3Connection::wait_closed() noexcept {
    co_await peer_reader_group_.join();
    mark_closed();
}

void Http3Connection::handle_peer_stream_attached(quic::QuicStream &stream) noexcept {
    if (closing() || state_ != Http3ConnectionState::Running) {
        stream.close(error_value(Http3ErrorCode::RequestCancelled));
        return;
    }

    if (stream.bidirectional()) {
        if (role() == quic::QuicConnectionRole::Client) {
            close(Http3ErrorCode::StreamCreationError);
            return;
        }
        stream.close(error_value(Http3ErrorCode::RequestRejected));
        return;
    }

    start_peer_uni_reader(stream);
}

void Http3Connection::start_peer_uni_reader(quic::QuicStream &stream) noexcept {
    quic::QuicStream::Lease lease = stream.lease();
    peer_reader_group_.add();
    event::EventLoop *loop = quic_.loop();
    FIBER_ASSERT(loop != nullptr);
    async::spawn(*loop, [this, lease = std::move(lease)]() mutable { return run_peer_uni_stream(std::move(lease)); });
}

async::DetachedTask Http3Connection::run_peer_uni_stream(quic::QuicStream::Lease stream) noexcept {
    if (!stream) {
        peer_reader_group_.done();
        co_return;
    }

    struct ReaderScope {
        Http3Connection &conn;
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
    mem::IoBufNodePool &pool = quic_.recv_extent_pool();
    mem::IoBufChain input(pool);

    Http3VarintParser stream_type_parser;
    for (;;) {
        Http3ParseStatus status = stream_type_parser.parse(input);
        if (status == Http3ParseStatus::Done) {
            break;
        }
        if (status == Http3ParseStatus::Error) {
            close_from_reader(Http3ErrorCode::StreamCreationError);
            co_return;
        }

        auto read = co_await reader.lease->read(kHttp3ReadChunkSize, input);
        if (!read) {
            if (read.error() != common::IoErr::Canceled || !stopping_readers_) {
                close_from_reader(Http3ErrorCode::StreamCreationError);
            }
            co_return;
        }
        if (*read == 0 && input.complete()) {
            close_from_reader(Http3ErrorCode::StreamCreationError);
            co_return;
        }
    }

    auto registered = register_peer_stream(reader, stream_type_parser.value());
    if (!registered) {
        if (registered.error() != common::IoErr::NotSupported) {
            close_from_reader(Http3ErrorCode::StreamCreationError);
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
                    close_from_reader(decoder.error().h3_error);
                    co_return;
                }
                if (status == Http3ParseStatus::Done) {
                    if (event.type == Http3ControlStreamEventType::Settings) {
                        auto applied = apply_peer_settings(event.settings);
                        if (!applied) {
                            close_from_reader(Http3ErrorCode::SettingsError);
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
                    close_from_reader(is_terminal_reader_error(read.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                             : Http3ErrorCode::FrameError);
                    co_return;
                }
                if (*read == 0 && input.complete()) {
                    close_from_reader(Http3ErrorCode::ClosedCriticalStream);
                    co_return;
                }
            }
        }

        case Http3StreamKind::QpackEncoder: {
            Http3QpackEncoderStreamDecoder decoder;
            for (;;) {
                Http3ParseStatus status = decoder.parse(input);
                if (status == Http3ParseStatus::Error) {
                    close_from_reader(decoder.error().h3_error);
                    co_return;
                }

                input.drop_empty_front();
                auto read = co_await reader.lease->read(kHttp3ReadChunkSize, input);
                if (!read) {
                    if (read.error() == common::IoErr::Canceled && stopping_readers_) {
                        co_return;
                    }
                    close_from_reader(is_terminal_reader_error(read.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                             : Http3ErrorCode::QpackEncoderStreamError);
                    co_return;
                }
                if (*read == 0 && input.complete()) {
                    close_from_reader(Http3ErrorCode::ClosedCriticalStream);
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
                    close_from_reader(decoder.error().h3_error);
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
                    close_from_reader(is_terminal_reader_error(read.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                             : Http3ErrorCode::QpackDecoderStreamError);
                    co_return;
                }
                if (*read == 0 && input.complete()) {
                    close_from_reader(Http3ErrorCode::ClosedCriticalStream);
                    co_return;
                }
            }
        }

        default:
            co_return;
    }
}

common::IoResult<void> Http3Connection::register_peer_stream(PeerStreamReader &reader,
                                                             std::uint64_t stream_type) noexcept {
    switch (static_cast<Http3StreamType>(stream_type)) {
        case Http3StreamType::Control:
            if (peer_control_seen_) {
                return std::unexpected(common::IoErr::Already);
            }
            peer_control_seen_ = true;
            peer_control_stream_ = reader.lease.get();
            reader.kind = Http3StreamKind::Control;
            return {};
        case Http3StreamType::QpackEncoder:
            if (peer_qpack_encoder_seen_) {
                return std::unexpected(common::IoErr::Already);
            }
            peer_qpack_encoder_seen_ = true;
            peer_qpack_encoder_stream_ = reader.lease.get();
            reader.kind = Http3StreamKind::QpackEncoder;
            return {};
        case Http3StreamType::QpackDecoder:
            if (peer_qpack_decoder_seen_) {
                return std::unexpected(common::IoErr::Already);
            }
            peer_qpack_decoder_seen_ = true;
            peer_qpack_decoder_stream_ = reader.lease.get();
            reader.kind = Http3StreamKind::QpackDecoder;
            return {};
        default:
            reader.kind = Http3StreamKind::UnknownUni;
            return std::unexpected(common::IoErr::NotSupported);
    }
}

void Http3Connection::unregister_peer_stream(PeerStreamReader &reader) noexcept {
    quic::QuicStream *stream = reader.lease.get();
    if (reader.kind == Http3StreamKind::Control && peer_control_stream_ == stream) {
        peer_control_stream_ = nullptr;
    } else if (reader.kind == Http3StreamKind::QpackEncoder && peer_qpack_encoder_stream_ == stream) {
        peer_qpack_encoder_stream_ = nullptr;
    } else if (reader.kind == Http3StreamKind::QpackDecoder && peer_qpack_decoder_stream_ == stream) {
        peer_qpack_decoder_stream_ = nullptr;
    }
    peer_readers_.erase(reader);
}

void Http3Connection::stop_peer_readers(Http3ErrorCode error) noexcept {
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

void Http3Connection::close_from_reader(Http3ErrorCode error) noexcept { close(error); }

} // namespace fiber::http
