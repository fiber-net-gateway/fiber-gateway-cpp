#include <fiber/http/Http3Connection.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <utility>

#include <fiber/async/Timeout.h>
#include <fiber/common/Assert.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http3Codec.h>
#include <fiber/http/Http3ControlStreamDecoder.h>
#include <fiber/http/Http3ControlStreamEncoder.h>
#include <fiber/http/Http3QpackControlStreamDecoder.h>
#include <fiber/http/ServerHttp3Request.h>

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
    FIBER_ASSERT(client_request_group_.empty());
    FIBER_ASSERT(client_requests_.empty());
    FIBER_ASSERT(control_task_group_.empty());
}

const quic::QuicConnection::Ops &Http3Connection::quic_ops() noexcept {
    static const quic::QuicConnection::Ops kOps{
            .create_stream = &Http3Connection::create_peer_stream,
            .on_peer_stream_attached = &Http3Connection::on_peer_stream_attached,
    };
    return kOps;
}

quic::QuicStream::Lease Http3Connection::create_owned_stream() noexcept {
    auto *stream = new (std::nothrow) quic::QuicStream(nullptr, &Http3Connection::destroy_peer_stream);
    return quic::QuicStream::Lease::adopt(stream);
}

quic::QuicStream::Lease Http3Connection::create_peer_stream(void *owner, std::uint64_t stream_id) noexcept {
    auto *conn = static_cast<Http3Connection *>(owner);
    if (conn != nullptr && quic::QuicStream::is_bidirectional_stream_id(stream_id) &&
        conn->role() == quic::QuicConnectionRole::Server && conn->options_.ops.create_server_request != nullptr) {
        return conn->options_.ops.create_server_request(conn->options_.owner, stream_id, *conn);
    }

    return create_owned_stream();
}

void Http3Connection::destroy_peer_stream(void *, quic::QuicStream &stream) noexcept { delete &stream; }

void Http3Connection::on_peer_stream_attached(void *owner, quic::QuicStream &stream) noexcept {
    auto *conn = static_cast<Http3Connection *>(owner);
    FIBER_ASSERT(conn != nullptr);
    conn->handle_peer_stream_attached(stream);
}

common::IoResult<void> Http3Connection::prepare() noexcept {
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
    state_ = Http3ConnectionState::Prepared;
    return {};
}

async::Task<common::IoResult<void>> Http3Connection::start() noexcept {
    if (state_ == Http3ConnectionState::Init) {
        auto prepared = prepare();
        if (!prepared) {
            co_return std::unexpected(prepared.error());
        }
    }
    if (state_ != Http3ConnectionState::Prepared) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (quic_.closing()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    auto preface = encode_http3_control_stream_preface(options_.local_settings, quic_.recv_extent_pool());
    if (!preface) {
        co_return std::unexpected(preface.error());
    }

    state_ = Http3ConnectionState::Starting;

    quic::QuicStream::Lease control_stream = create_owned_stream();
    if (!control_stream) {
        close(Http3ErrorCode::InternalError);
        co_return std::unexpected(common::IoErr::NoMem);
    }

    auto attached = co_await quic_.attach_local_stream(std::move(control_stream), quic::QuicStreamType::Unidirectional);
    if (!attached) {
        close(Http3ErrorCode::StreamCreationError);
        co_return std::unexpected(attached.error());
    }

    local_control_stream_ = (*attached)->lease();
    mem::IoBufChain control_preface = std::move(*preface);
    while (!control_preface.empty()) {
        auto written = co_await local_control_stream_->write(control_preface);
        if (!written) {
            close(Http3ErrorCode::ClosedCriticalStream);
            co_return std::unexpected(written.error());
        }
        if (*written == 0 && !control_preface.empty()) {
            close(Http3ErrorCode::ClosedCriticalStream);
            co_return std::unexpected(common::IoErr::WouldBlock);
        }
    }

    if (state_ == Http3ConnectionState::Starting) {
        state_ = Http3ConnectionState::Running;
    }
    co_return common::IoResult<void>{};
}

common::IoResult<void> Http3Connection::apply_peer_settings(const Http3Settings &settings) noexcept {
    if (state_ == Http3ConnectionState::Closing || state_ == Http3ConnectionState::Closed) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (peer_settings_received_) {
        return std::unexpected(common::IoErr::Already);
    }
    peer_settings_ = settings;
    peer_settings_received_ = true;
    return {};
}

common::IoResult<void> Http3Connection::register_client_request(Http3ClientRequestEntry &entry) noexcept {
    if (!accepting_requests() || entry.link.linked() || entry.owner == nullptr || entry.on_rejected == nullptr ||
        entry.on_connection_close == nullptr || entry.stream_id == quic::kQuicUnassignedStreamId ||
        !quic::QuicStream::is_bidirectional_stream_id(entry.stream_id)) {
        return std::unexpected(accepting_requests() ? common::IoErr::Invalid : common::IoErr::Canceled);
    }
    client_requests_.push_back(entry);
    client_request_group_.add();
    return {};
}

void Http3Connection::unregister_client_request(Http3ClientRequestEntry &entry) noexcept {
    if (!entry.link.linked()) {
        return;
    }
    client_requests_.erase(entry);
    client_request_group_.done();
}

void Http3Connection::graceful_shutdown(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closing || state_ == Http3ConnectionState::Closed ||
        (state_ == Http3ConnectionState::Draining && (local_goaway_sent_ || !control_task_group_.empty()))) {
        return;
    }
    close_error_ = error;
    state_ = Http3ConnectionState::Draining;
    if (role() != quic::QuicConnectionRole::Client || !local_control_stream_) {
        close(error);
        return;
    }

    event::EventLoop *loop = quic_.loop();
    FIBER_ASSERT(loop != nullptr);
    control_task_group_.add();
    async::spawn(*loop, [this, error]() -> async::DetachedTask { return run_client_graceful_shutdown(error); });
}

void Http3Connection::close(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closing || state_ == Http3ConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = Http3ConnectionState::Closing;
    detach_client_requests(error);
    stop_peer_readers(error);
    quic_.close_application(error_value(error));
}

void Http3Connection::mark_closed() noexcept { state_ = Http3ConnectionState::Closed; }

async::Task<void> Http3Connection::wait_closed() noexcept {
    co_await peer_reader_group_.join();
    co_await control_task_group_.join();
    mark_closed();
}

void Http3Connection::handle_peer_stream_attached(quic::QuicStream &stream) noexcept {
    if (stream.bidirectional()) {
        if (role() == quic::QuicConnectionRole::Client) {
            close(Http3ErrorCode::StreamCreationError);
            return;
        }
        if (state_ != Http3ConnectionState::Prepared && state_ != Http3ConnectionState::Starting &&
            state_ != Http3ConnectionState::Running) {
            stream.close(error_value(Http3ErrorCode::RequestRejected));
            return;
        }
        ServerHttp3Request *request = ServerHttp3Request::from_stream(stream);
        if (request == nullptr) {
            stream.close(error_value(Http3ErrorCode::RequestRejected));
            return;
        }
        event::EventLoop *loop = quic_.loop();
        FIBER_ASSERT(loop != nullptr);
        request->start_read_loop(*loop);
        return;
    }

    if (state_ == Http3ConnectionState::Closing || state_ == Http3ConnectionState::Closed) {
        stream.close(error_value(Http3ErrorCode::RequestCancelled));
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
    mem::IoBufChain input;

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

    if (stream_type_parser.value() == static_cast<std::uint64_t>(Http3StreamType::Push)) {
        close_from_reader(role() == quic::QuicConnectionRole::Client ? Http3ErrorCode::IdError
                                                                     : Http3ErrorCode::StreamCreationError);
        co_return;
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
                    } else if (event.type == Http3ControlStreamEventType::Goaway) {
                        auto applied = apply_peer_goaway(event.id);
                        if (!applied) {
                            close_from_reader(Http3ErrorCode::IdError);
                            co_return;
                        }
                    } else if (event.type == Http3ControlStreamEventType::MaxPushId &&
                               role() == quic::QuicConnectionRole::Client) {
                        close_from_reader(Http3ErrorCode::FrameUnexpected);
                        co_return;
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

common::IoResult<void> Http3Connection::apply_peer_goaway(std::uint64_t id) noexcept {
    if (role() != quic::QuicConnectionRole::Client) {
        return {};
    }
    if ((id & 0x03U) != 0 || (peer_goaway_received_ && id > peer_goaway_id_)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    peer_goaway_received_ = true;
    peer_goaway_id_ = id;
    if (state_ != Http3ConnectionState::Closing && state_ != Http3ConnectionState::Closed) {
        state_ = Http3ConnectionState::Draining;
    }
    reject_client_requests(id);
    return {};
}

void Http3Connection::reject_client_requests(std::uint64_t goaway_id) noexcept {
    Http3ClientRequestEntry *entry = client_requests_.front();
    while (entry != nullptr) {
        Http3ClientRequestEntry *next = client_requests_.next_of(*entry);
        if (entry->stream_id >= goaway_id) {
            client_requests_.erase(*entry);
            client_request_group_.done();
            entry->on_rejected(entry->owner, goaway_id);
        }
        entry = next;
    }
}

void Http3Connection::detach_client_requests(Http3ErrorCode error) noexcept {
    Http3ClientRequestEntry *entry = client_requests_.front();
    while (entry != nullptr) {
        Http3ClientRequestEntry *next = client_requests_.next_of(*entry);
        client_requests_.erase(*entry);
        client_request_group_.done();
        entry->on_connection_close(entry->owner, error);
        entry = next;
    }
}

async::DetachedTask Http3Connection::run_client_graceful_shutdown(Http3ErrorCode error) noexcept {
    auto frame = encode_http3_goaway_frame(0, quic_.recv_extent_pool());
    if (!frame) {
        close(Http3ErrorCode::InternalError);
        control_task_group_.done();
        co_return;
    }

    while (!frame->empty()) {
        auto written = co_await local_control_stream_->write(*frame);
        if (!written || (*written == 0 && !frame->empty())) {
            close(Http3ErrorCode::ClosedCriticalStream);
            control_task_group_.done();
            co_return;
        }
    }
    local_goaway_sent_ = true;

    (void) co_await async::timeout_for([this]() { return client_request_group_.join(); }, options_.drain_timeout);
    if (state_ == Http3ConnectionState::Draining) {
        close(error);
    }
    control_task_group_.done();
    co_return;
}

} // namespace fiber::http
