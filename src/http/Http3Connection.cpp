#include "Http3Connection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>

#include "../common/Assert.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::http {

namespace {

constexpr std::size_t kHttp3DrainChunkSize = 4096;

struct StreamByte {
    std::uint8_t value = 0;
    bool eof = false;
};

struct StreamVarint {
    std::uint64_t value = 0;
    bool eof = false;
};

[[nodiscard]] std::uint64_t error_value(Http3ErrorCode error) noexcept { return static_cast<std::uint64_t>(error); }

[[nodiscard]] bool is_terminal_reader_error(common::IoErr err) noexcept {
    return err == common::IoErr::Canceled || err == common::IoErr::ConnReset || err == common::IoErr::BrokenPipe;
}

async::Task<common::IoResult<StreamByte>> read_stream_byte(quic::QuicStream &stream,
                                                           mem::IoBufNodePool &pool) noexcept {
    mem::IoBufChain chunk(pool);
    auto read = co_await stream.read(1, chunk);
    if (!read) {
        co_return std::unexpected(read.error());
    }
    if (*read == 0) {
        if (chunk.complete()) {
            co_return StreamByte{.value = 0, .eof = true};
        }
        co_return std::unexpected(common::IoErr::Invalid);
    }

    mem::IoBuf *buf = chunk.first_readable();
    if (buf == nullptr || buf->readable() == 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return StreamByte{.value = *buf->readable_data(), .eof = false};
}

async::Task<common::IoResult<StreamVarint>> read_stream_varint(quic::QuicStream &stream,
                                                               mem::IoBufNodePool &pool) noexcept {
    auto first = co_await read_stream_byte(stream, pool);
    if (!first) {
        co_return std::unexpected(first.error());
    }
    if (first->eof) {
        co_return StreamVarint{.value = 0, .eof = true};
    }

    const std::uint8_t len = static_cast<std::uint8_t>(1U << (first->value >> 6U));
    std::uint64_t value = first->value & 0x3fU;
    for (std::uint8_t i = 1; i < len; ++i) {
        auto byte = co_await read_stream_byte(stream, pool);
        if (!byte) {
            co_return std::unexpected(byte.error());
        }
        if (byte->eof) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        value = (value << 8U) | byte->value;
    }
    co_return StreamVarint{.value = value, .eof = false};
}

async::Task<common::IoResult<std::uint64_t>> read_limited_varint(quic::QuicStream &stream, mem::IoBufNodePool &pool,
                                                                 std::uint64_t &remaining) noexcept {
    if (remaining == 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    auto first = co_await read_stream_byte(stream, pool);
    if (!first) {
        co_return std::unexpected(first.error());
    }
    if (first->eof) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    --remaining;

    const std::uint8_t len = static_cast<std::uint8_t>(1U << (first->value >> 6U));
    if (remaining < static_cast<std::uint64_t>(len - 1U)) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::uint64_t value = first->value & 0x3fU;
    for (std::uint8_t i = 1; i < len; ++i) {
        auto byte = co_await read_stream_byte(stream, pool);
        if (!byte) {
            co_return std::unexpected(byte.error());
        }
        if (byte->eof) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        --remaining;
        value = (value << 8U) | byte->value;
    }
    co_return value;
}

async::Task<common::IoResult<Http3FrameHeader>> read_frame_header(quic::QuicStream &stream,
                                                                  mem::IoBufNodePool &pool) noexcept {
    auto type = co_await read_stream_varint(stream, pool);
    if (!type) {
        co_return std::unexpected(type.error());
    }
    if (type->eof) {
        co_return std::unexpected(common::IoErr::BrokenPipe);
    }

    auto length = co_await read_stream_varint(stream, pool);
    if (!length) {
        co_return std::unexpected(length.error());
    }
    if (length->eof) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    co_return Http3FrameHeader{.type = type->value, .length = length->value};
}

async::Task<common::IoResult<void>> drain_payload(quic::QuicStream &stream, mem::IoBufNodePool &pool,
                                                  std::uint64_t length) noexcept {
    while (length != 0) {
        const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(length, static_cast<std::uint64_t>(kHttp3DrainChunkSize)));
        mem::IoBufChain chunk(pool);
        auto read = co_await stream.read(want, chunk);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read == 0) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        length -= *read;
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> parse_settings_payload(quic::QuicStream &stream, mem::IoBufNodePool &pool,
                                                           std::uint64_t length, Http3Settings &settings) noexcept {
    bool saw_qpack_max_table_capacity = false;
    bool saw_qpack_blocked_streams = false;
    bool saw_max_field_section_size = false;
    bool saw_enable_connect_protocol = false;

    while (length != 0) {
        auto id = co_await read_limited_varint(stream, pool, length);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        auto value = co_await read_limited_varint(stream, pool, length);
        if (!value) {
            co_return std::unexpected(value.error());
        }

        switch (static_cast<Http3SettingId>(*id)) {
            case Http3SettingId::QpackMaxTableCapacity:
                if (saw_qpack_max_table_capacity) {
                    co_return std::unexpected(common::IoErr::Already);
                }
                saw_qpack_max_table_capacity = true;
                settings.qpack_max_table_capacity = *value;
                break;
            case Http3SettingId::QpackBlockedStreams:
                if (saw_qpack_blocked_streams) {
                    co_return std::unexpected(common::IoErr::Already);
                }
                saw_qpack_blocked_streams = true;
                settings.qpack_blocked_streams = *value;
                break;
            case Http3SettingId::MaxFieldSectionSize:
                if (saw_max_field_section_size) {
                    co_return std::unexpected(common::IoErr::Already);
                }
                saw_max_field_section_size = true;
                settings.max_field_section_size = *value;
                break;
            case Http3SettingId::EnableConnectProtocol:
                if (saw_enable_connect_protocol || *value > 1) {
                    co_return std::unexpected(common::IoErr::Already);
                }
                saw_enable_connect_protocol = true;
                settings.enable_connect_protocol = *value == 1;
                break;
            default:
                break;
        }
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<std::uint64_t>> read_qpack_prefixed_integer(quic::QuicStream &stream,
                                                                         mem::IoBufNodePool &pool, std::uint8_t first,
                                                                         std::uint8_t prefix_bits) noexcept {
    FIBER_ASSERT(prefix_bits > 0 && prefix_bits < 8);
    const std::uint64_t prefix_max = (1ULL << prefix_bits) - 1ULL;
    std::uint64_t value = first & prefix_max;
    if (value < prefix_max) {
        co_return value;
    }

    std::uint8_t shift = 0;
    for (;;) {
        auto byte = co_await read_stream_byte(stream, pool);
        if (!byte) {
            co_return std::unexpected(byte.error());
        }
        if (byte->eof || shift >= 63U) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        const std::uint64_t chunk = byte->value & 0x7fU;
        if (chunk > (std::numeric_limits<std::uint64_t>::max() >> shift)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        value += chunk << shift;
        if ((byte->value & 0x80U) == 0) {
            co_return value;
        }
        shift = static_cast<std::uint8_t>(shift + 7U);
    }
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

    auto stream_type = co_await read_stream_varint(*reader.lease, pool);
    if (!stream_type) {
        if (stream_type.error() != common::IoErr::Canceled || !stopping_readers_) {
            close_from_reader(Http3ErrorCode::StreamCreationError);
        }
        co_return;
    }
    if (stream_type->eof) {
        close_from_reader(Http3ErrorCode::StreamCreationError);
        co_return;
    }

    auto registered = register_peer_stream(reader, stream_type->value);
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
            bool first_frame = true;
            for (;;) {
                auto header = co_await read_frame_header(*reader.lease, pool);
                if (!header) {
                    if (header.error() == common::IoErr::Canceled && stopping_readers_) {
                        co_return;
                    }
                    close_from_reader(header.error() == common::IoErr::BrokenPipe ||
                                                      is_terminal_reader_error(header.error())
                                              ? Http3ErrorCode::ClosedCriticalStream
                                              : Http3ErrorCode::FrameError);
                    co_return;
                }

                if (first_frame && header->type != static_cast<std::uint64_t>(Http3FrameType::Settings)) {
                    close_from_reader(Http3ErrorCode::MissingSettings);
                    co_return;
                }
                first_frame = false;

                switch (static_cast<Http3FrameType>(header->type)) {
                    case Http3FrameType::Settings: {
                        if (peer_settings_received_) {
                            close_from_reader(Http3ErrorCode::FrameUnexpected);
                            co_return;
                        }
                        Http3Settings settings{};
                        auto parsed = co_await parse_settings_payload(*reader.lease, pool, header->length, settings);
                        if (!parsed) {
                            close_from_reader(Http3ErrorCode::SettingsError);
                            co_return;
                        }
                        auto applied = apply_peer_settings(settings);
                        if (!applied) {
                            close_from_reader(Http3ErrorCode::SettingsError);
                            co_return;
                        }
                        break;
                    }
                    case Http3FrameType::Data:
                    case Http3FrameType::Headers:
                    case Http3FrameType::PushPromise:
                        close_from_reader(Http3ErrorCode::FrameUnexpected);
                        co_return;
                    default: {
                        auto drained = co_await drain_payload(*reader.lease, pool, header->length);
                        if (!drained) {
                            if (drained.error() == common::IoErr::Canceled && stopping_readers_) {
                                co_return;
                            }
                            close_from_reader(Http3ErrorCode::FrameError);
                            co_return;
                        }
                        break;
                    }
                }
            }
        }

        case Http3StreamKind::QpackEncoder:
            for (;;) {
                auto byte = co_await read_stream_byte(*reader.lease, pool);
                if (!byte) {
                    if (byte.error() == common::IoErr::Canceled && stopping_readers_) {
                        co_return;
                    }
                    close_from_reader(is_terminal_reader_error(byte.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                             : Http3ErrorCode::QpackEncoderStreamError);
                    co_return;
                }
                if (byte->eof) {
                    close_from_reader(Http3ErrorCode::ClosedCriticalStream);
                    co_return;
                }

                if ((byte->value & 0xe0U) != 0x20U) {
                    close_from_reader(Http3ErrorCode::QpackEncoderStreamError);
                    co_return;
                }
                auto capacity = co_await read_qpack_prefixed_integer(*reader.lease, pool, byte->value, 5);
                if (!capacity || *capacity != 0) {
                    close_from_reader(Http3ErrorCode::QpackEncoderStreamError);
                    co_return;
                }
            }

        case Http3StreamKind::QpackDecoder: {
            auto byte = co_await read_stream_byte(*reader.lease, pool);
            if (!byte) {
                if (byte.error() == common::IoErr::Canceled && stopping_readers_) {
                    co_return;
                }
                close_from_reader(is_terminal_reader_error(byte.error()) ? Http3ErrorCode::ClosedCriticalStream
                                                                         : Http3ErrorCode::QpackDecoderStreamError);
                co_return;
            }
            if (byte->eof) {
                close_from_reader(Http3ErrorCode::ClosedCriticalStream);
                co_return;
            }
            close_from_reader(Http3ErrorCode::QpackDecoderStreamError);
            co_return;
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
