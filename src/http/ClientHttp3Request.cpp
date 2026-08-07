#include <fiber/http/ClientHttp3Request.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <expected>
#include <limits>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/Http3FrameWriter.h>
#include <fiber/http/Http3QpackEncoderIoBufWriter.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/detail/Http2HeaderDecodeUtil.h>

namespace fiber::http {

namespace {

constexpr std::size_t kHttp3ReadChunkSize = 16 * 1024;

using TimePoint = std::chrono::steady_clock::time_point;

TimePoint deadline_after(std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return TimePoint::max();
    }
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }
    return event::EventLoop::current().now() + timeout;
}

std::chrono::milliseconds remaining_timeout(TimePoint deadline) noexcept {
    if (deadline == TimePoint::max()) {
        return std::chrono::milliseconds::max();
    }
    const TimePoint now = event::EventLoop::current().now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

[[nodiscard]] std::uint64_t error_value(Http3ErrorCode error) noexcept { return static_cast<std::uint64_t>(error); }

[[nodiscard]] bool is_informational(int status) noexcept { return status >= 100 && status < 200; }

[[nodiscard]] bool is_valid_header_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (char ch: name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            continue;
        }
        switch (c) {
            case '!':
            case '#':
            case '$':
            case '%':
            case '&':
            case '\'':
            case '*':
            case '+':
            case '-':
            case '.':
            case '^':
            case '_':
            case '`':
            case '|':
            case '~':
                continue;
            default:
                break;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool is_valid_header_value(std::string_view value) noexcept {
    if (!value.empty() &&
        (value.front() == ' ' || value.front() == '\t' || value.back() == ' ' || value.back() == '\t')) {
        return false;
    }
    for (char ch: value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == 0x7fU || (c < 0x20U && c != '\t')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_forbidden_connection_header(std::string_view name) noexcept {
    return name == "connection" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding" ||
           name == "upgrade";
}

[[nodiscard]] bool is_forbidden_request_stream_frame(std::uint64_t type) noexcept {
    switch (static_cast<Http3FrameType>(type)) {
        case Http3FrameType::CancelPush:
        case Http3FrameType::Settings:
        case Http3FrameType::PushPromise:
        case Http3FrameType::Goaway:
        case Http3FrameType::MaxPushId:
            return true;
        case Http3FrameType::Data:
        case Http3FrameType::Headers:
            return false;
    }
    return false;
}

[[nodiscard]] common::IoResult<std::size_t> parse_content_length(std::string_view value) noexcept {
    if (value.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::size_t parsed = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return parsed;
}

[[nodiscard]] bool response_must_not_have_body(HttpMethod method, int status) noexcept {
    return method == HttpMethod::Head || status == 204 || status == 304;
}

[[nodiscard]] std::string_view method_value(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::MKCOL:
            return "MKCOL";
        case HttpMethod::Copy:
            return "COPY";
        case HttpMethod::Move:
            return "MOVE";
        case HttpMethod::Options:
            return "OPTIONS";
        case HttpMethod::PropFind:
            return "PROPFIND";
        case HttpMethod::PropPatch:
            return "PROPPATCH";
        case HttpMethod::Lock:
            return "LOCK";
        case HttpMethod::Unlock:
            return "UNLOCK";
        case HttpMethod::Patch:
            return "PATCH";
        case HttpMethod::Trace:
            return "TRACE";
        case HttpMethod::Connect:
            return "CONNECT";
        case HttpMethod::Unknown:
            return {};
    }
    return {};
}

[[nodiscard]] bool account_outgoing_field(std::uint64_t &total, std::string_view name, std::string_view value,
                                          std::uint64_t limit) noexcept {
    constexpr std::uint64_t kFieldOverhead = 32;
    const auto name_size = static_cast<std::uint64_t>(name.size());
    const auto value_size = static_cast<std::uint64_t>(value.size());
    if (value_size > kMaxHttp3FramePayloadLength - kFieldOverhead ||
        name_size > kMaxHttp3FramePayloadLength - kFieldOverhead - value_size) {
        return false;
    }
    const std::uint64_t size = name_size + value_size + kFieldOverhead;
    if (total > kMaxHttp3FramePayloadLength - size) {
        return false;
    }
    total += size;
    return limit == 0 || total <= limit;
}

class BoolOperationGuard {
public:
    explicit BoolOperationGuard(bool &active) noexcept : active_(&active) { active = true; }
    ~BoolOperationGuard() {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

    void release() noexcept {
        if (active_ != nullptr) {
            *active_ = false;
            active_ = nullptr;
        }
    }

private:
    bool *active_ = nullptr;
};

} // namespace

ClientHttp3Request::ClientHttp3Request(Http3Connection &conn, mem::BufPool &pool) noexcept :
    quic_lease_(conn.quic().lease()), conn_(&conn), stream_(this, &ClientHttp3Request::destroy_owner), pool_(&pool),
    inbound_buf_(conn.quic().recv_extent_pool()),
    request_entry_{.owner = this,
                   .on_rejected = &ClientHttp3Request::on_rejected,
                   .on_connection_close = &ClientHttp3Request::on_connection_close} {
    (void) qpack_decoder_.init(conn.max_qpack_string_size());
}

ClientHttp3Request::~ClientHttp3Request() {
    finish_observation();
    qpack_decoder_.release();
}

quic::QuicStream::Lease ClientHttp3Request::create(Http3Connection &conn, mem::BufPool &pool) noexcept {
    auto *request = new (std::nothrow) ClientHttp3Request(conn, pool);
    return request == nullptr ? quic::QuicStream::Lease{} : quic::QuicStream::Lease::adopt(&request->stream_);
}

ClientHttp3Request *ClientHttp3Request::from_stream(quic::QuicStream &stream) noexcept {
    if (stream.destroy_callback() != &ClientHttp3Request::destroy_owner) {
        return nullptr;
    }
    auto *request = static_cast<ClientHttp3Request *>(stream.owner());
    return request != nullptr && &request->stream_ == &stream ? request : nullptr;
}

const ClientHttp3Request *ClientHttp3Request::from_stream(const quic::QuicStream &stream) noexcept {
    if (stream.destroy_callback() != &ClientHttp3Request::destroy_owner) {
        return nullptr;
    }
    auto *request = static_cast<const ClientHttp3Request *>(stream.owner());
    return request != nullptr && &request->stream_ == &stream ? request : nullptr;
}

void ClientHttp3Request::destroy_owner(void *owner, quic::QuicStream &) noexcept {
    delete static_cast<ClientHttp3Request *>(owner);
}

common::IoResult<void> ClientHttp3Request::register_attached() noexcept {
    if (conn_ == nullptr || !stream_.stream_id_assigned()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    request_entry_.stream_id = stream_.stream_id();
    return conn_->register_client_request(request_entry_);
}

mem::IoBufNodePool &ClientHttp3Request::node_pool() noexcept {
    FIBER_ASSERT(conn_ != nullptr);
    return conn_->quic().recv_extent_pool();
}

Http3ExtendedConnectSupport ClientHttp3Request::extended_connect_support() const noexcept {
    if (conn_ == nullptr || !conn_->peer_settings_received()) {
        return Http3ExtendedConnectSupport::Unknown;
    }
    return conn_->peer_settings().enable_connect_protocol ? Http3ExtendedConnectSupport::Enabled
                                                          : Http3ExtendedConnectSupport::Disabled;
}

async::Task<common::IoResult<void>> ClientHttp3Request::write_frame(mem::IoBufChain &frame,
                                                                    std::chrono::milliseconds timeout) noexcept {
    const TimePoint deadline = deadline_after(timeout);
    while (frame.readable_bytes() != 0 || frame.complete()) {
        auto written = co_await stream_.write(frame, remaining_timeout(deadline));
        if (!written) {
            handle_io_error(written.error());
            co_return std::unexpected(written.error());
        }
        if (*written == 0 && (frame.readable_bytes() != 0 || frame.complete())) {
            handle_io_error(common::IoErr::WouldBlock);
            co_return std::unexpected(common::IoErr::WouldBlock);
        }
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>>
ClientHttp3Request::write_data_frame_header(std::size_t payload_len, std::chrono::milliseconds timeout) noexcept {
    auto header = http3_build_data_frame_header(payload_len);
    if (!header) {
        co_return std::unexpected(header.error());
    }
    if (!*header) {
        co_return common::IoResult<void>{};
    }

    mem::IoBufChain frame(node_pool());
    if (!frame.append(std::move(*header))) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    co_return co_await write_frame(frame, timeout);
}

async::Task<common::IoResult<void>>
ClientHttp3Request::send_request_header(const Http3RequestHead &head, bool end_stream,
                                        std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr || !stream_.stream_id_assigned() || !request_entry_.link.linked()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (writing_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    BoolOperationGuard guard(writing_);
    if (request_headers_sent_ || request_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (head.method == HttpMethod::Unknown) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (head.protocol.empty()) {
        if (head.method == HttpMethod::Connect) {
            if (head.authority.empty() || !head.scheme.empty() || !head.path.empty()) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
        } else if (head.scheme.empty() || head.path.empty()) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    } else if (head.method != HttpMethod::Connect || head.scheme.empty() || head.authority.empty() ||
               head.path.empty()) {
        co_return std::unexpected(common::IoErr::Invalid);
    } else if (extended_connect_support() == Http3ExtendedConnectSupport::Unknown) {
        co_return std::unexpected(common::IoErr::WouldBlock);
    } else if (extended_connect_support() == Http3ExtendedConnectSupport::Disabled) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }

    bool content_length_seen = false;
    std::size_t content_length = 0;
    const std::uint64_t peer_field_limit =
            conn_->peer_settings_received() ? conn_->peer_settings().max_field_section_size : 0;
    std::uint64_t field_section_size = 0;
    if (!account_outgoing_field(field_section_size, ":method", method_value(head.method), peer_field_limit) ||
        (!head.scheme.empty() &&
         !account_outgoing_field(field_section_size, ":scheme", head.scheme, peer_field_limit)) ||
        (!head.authority.empty() &&
         !account_outgoing_field(field_section_size, ":authority", head.authority, peer_field_limit)) ||
        (!head.path.empty() && !account_outgoing_field(field_section_size, ":path", head.path, peer_field_limit)) ||
        (!head.protocol.empty() &&
         !account_outgoing_field(field_section_size, ":protocol", head.protocol, peer_field_limit))) {
        co_return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if (head.headers != nullptr) {
        for (const auto &field: *head.headers) {
            std::string_view name = field.lowcase_view();
            if (name.empty()) {
                name = field.name_view();
            }
            const std::string_view value = field.value_view();
            if (!is_valid_header_name(name) || !is_valid_header_value(value) || name.front() == ':' ||
                is_forbidden_connection_header(name) || (name == "te" && value != "trailers")) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            if (!account_outgoing_field(field_section_size, name, value, peer_field_limit)) {
                co_return std::unexpected(common::IoErr::MessageTooLarge);
            }
            if (name == "content-length") {
                auto parsed = parse_content_length(value);
                if (!parsed || (content_length_seen && content_length != *parsed)) {
                    co_return std::unexpected(common::IoErr::Invalid);
                }
                content_length_seen = true;
                content_length = *parsed;
            }
        }
    }
    if (end_stream && content_length_seen && content_length != 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    Http3QpackEncoderIoBufWriter writer(node_pool(),
                                        Http3QpackEncoder::Options{.max_string_size = conn_->max_qpack_string_size()},
                                        512, kHttp3FrameHeaderReserve);
    common::IoErr err = writer.encode_method(head.method);
    if (err == common::IoErr::None && !head.scheme.empty()) {
        err = writer.encode_scheme(head.scheme);
    }
    if (err == common::IoErr::None && !head.authority.empty()) {
        err = writer.encode_authority(head.authority);
    }
    if (err == common::IoErr::None && !head.path.empty()) {
        err = writer.encode_path(head.path);
    }
    if (err == common::IoErr::None && !head.protocol.empty()) {
        err = writer.encode_field(":protocol", http_header_name_hash(":protocol"), head.protocol);
    }
    if (err != common::IoErr::None) {
        writer.abort();
        co_return std::unexpected(err);
    }

    if (head.headers != nullptr) {
        for (const auto &field: *head.headers) {
            std::string_view name = field.lowcase_view();
            if (name.empty()) {
                name = field.name_view();
            }
            err = writer.encode_field(name, field.name_hash, field.value_view());
            if (err != common::IoErr::None) {
                writer.abort();
                co_return std::unexpected(err);
            }
        }
    }

    mem::IoBufChain frame(node_pool());
    auto finished = http3_finish_headers_frame(writer, frame, end_stream);
    if (!finished) {
        co_return std::unexpected(finished.error());
    }

    request_method_ = head.method;
    request_content_length_seen_ = content_length_seen;
    request_content_length_ = content_length;
    outcome_ = Http3RequestOutcome::PossiblyProcessed;
    auto written = co_await write_frame(frame, timeout);
    if (!written) {
        (void) abort(written.error());
        co_return written;
    }

    request_headers_sent_ = true;
    request_finished_ = end_stream;
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<std::size_t>> ClientHttp3Request::write_all(mem::IoBufChain chunk,
                                                                         std::chrono::milliseconds timeout) noexcept {
    if (terminal_error_ != common::IoErr::None) {
        co_return std::unexpected(terminal_error_);
    }
    if (conn_ == nullptr || !request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (writing_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    BoolOperationGuard guard(writing_);
    if (request_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const std::size_t body_len = chunk.readable_bytes();
    const bool end_stream = chunk.complete();
    if (static_cast<std::uint64_t>(body_len) > kMaxHttp3FramePayloadLength) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_content_length_seen_) {
        if (request_body_sent_ > request_content_length_ || body_len > request_content_length_ - request_body_sent_ ||
            (end_stream && body_len != request_content_length_ - request_body_sent_)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    const TimePoint deadline = deadline_after(timeout);
    if (request_data_frame_active_) {
        if (body_len != request_data_frame_remaining_ || end_stream != request_data_frame_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        guard.release();
        std::size_t total = 0;
        while (request_data_frame_active_ || chunk.readable_bytes() != 0 || chunk.complete()) {
            auto written = co_await write(chunk, remaining_timeout(deadline));
            if (!written) {
                co_return std::unexpected(written.error());
            }
            total += *written;
        }
        co_return total;
    }

    if (body_len == 0) {
        if (!end_stream) {
            co_return 0;
        }
        auto written = co_await stream_.write(nullptr, 0, true, remaining_timeout(deadline));
        if (!written) {
            (void) abort(written.error());
            co_return std::unexpected(written.error());
        }
        chunk.clear_complete();
        request_finished_ = true;
        co_return 0;
    }

    auto prepared = http3_prepare_data_frame(chunk, node_pool());
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto written = co_await write_frame(chunk, remaining_timeout(deadline));
    if (!written) {
        (void) abort(written.error());
        co_return std::unexpected(written.error());
    }
    request_body_sent_ += body_len;
    request_finished_ = end_stream;
    co_return body_len;
}

async::Task<common::IoResult<std::size_t>> ClientHttp3Request::write(mem::IoBufChain &chunk,
                                                                     std::chrono::milliseconds timeout) noexcept {
    if (terminal_error_ != common::IoErr::None) {
        co_return std::unexpected(terminal_error_);
    }
    if (conn_ == nullptr || !request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (writing_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    BoolOperationGuard guard(writing_);
    if (request_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const std::size_t body_len = chunk.readable_bytes();
    const bool end_stream = chunk.complete();
    if (static_cast<std::uint64_t>(body_len) > kMaxHttp3FramePayloadLength) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_content_length_seen_) {
        if (request_body_sent_ > request_content_length_ || body_len > request_content_length_ - request_body_sent_ ||
            (end_stream && body_len != request_content_length_ - request_body_sent_)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    const TimePoint deadline = deadline_after(timeout);
    if (request_data_frame_active_) {
        if (body_len != request_data_frame_remaining_ || end_stream != request_data_frame_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    } else {
        if (body_len == 0) {
            if (!end_stream) {
                co_return 0;
            }
            auto written = co_await stream_.write(nullptr, 0, true, remaining_timeout(deadline));
            if (!written) {
                (void) abort(written.error());
                co_return std::unexpected(written.error());
            }
            chunk.clear_complete();
            request_finished_ = true;
            co_return 0;
        }
        auto header = co_await write_data_frame_header(body_len, remaining_timeout(deadline));
        if (!header) {
            if (terminal_error_ != common::IoErr::None) {
                (void) abort(header.error());
            }
            co_return std::unexpected(header.error());
        }
        request_data_frame_active_ = true;
        request_data_frame_remaining_ = body_len;
        request_data_frame_end_ = end_stream;
    }

    mem::IoBufChain staged;
    mem::IoBufChain *payload = &chunk;
    bool consume_borrowed_chain = false;
    if (!chunk.bound() || &chunk.node_pool() != &node_pool()) {
        staged.bind_node_pool(node_pool());
        if (!chunk.retain_prefix(body_len, staged)) {
            (void) abort(common::IoErr::NoMem);
            co_return std::unexpected(common::IoErr::NoMem);
        }
        payload = &staged;
        consume_borrowed_chain = true;
    }

    auto written = co_await stream_.write(*payload, remaining_timeout(deadline));
    if (!written || *written == 0) {
        const common::IoErr error = written ? common::IoErr::WouldBlock : written.error();
        (void) abort(error);
        co_return std::unexpected(error);
    }
    const std::size_t accepted = *written;
    if (consume_borrowed_chain) {
        chunk.consume_and_compact(accepted);
        if (accepted == body_len && end_stream) {
            chunk.clear_complete();
        }
    } else if (accepted == body_len && end_stream) {
        chunk.clear_complete();
    }

    request_data_frame_remaining_ -= accepted;
    request_body_sent_ += accepted;
    if (request_data_frame_remaining_ == 0) {
        request_data_frame_active_ = false;
        request_data_frame_end_ = false;
        if (end_stream) {
            request_finished_ = true;
        }
    }
    co_return accepted;
}

async::Task<common::IoResult<std::size_t>> ClientHttp3Request::write(const std::uint8_t *buf, std::size_t len,
                                                                     bool end_stream,
                                                                     std::chrono::milliseconds timeout) noexcept {
    if (terminal_error_ != common::IoErr::None) {
        co_return std::unexpected(terminal_error_);
    }
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (conn_ == nullptr || !request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (writing_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    BoolOperationGuard guard(writing_);
    if (request_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (static_cast<std::uint64_t>(len) > kMaxHttp3FramePayloadLength) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_content_length_seen_) {
        if (request_body_sent_ > request_content_length_ || len > request_content_length_ - request_body_sent_ ||
            (end_stream && len != request_content_length_ - request_body_sent_)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    const TimePoint deadline = deadline_after(timeout);
    if (request_data_frame_active_) {
        if (len != request_data_frame_remaining_ || end_stream != request_data_frame_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    } else {
        if (len == 0) {
            if (!end_stream) {
                co_return 0;
            }
            auto written = co_await stream_.write(nullptr, 0, true, remaining_timeout(deadline));
            if (!written) {
                (void) abort(written.error());
                co_return std::unexpected(written.error());
            }
            request_finished_ = true;
            co_return 0;
        }
        auto header = co_await write_data_frame_header(len, remaining_timeout(deadline));
        if (!header) {
            if (terminal_error_ != common::IoErr::None) {
                (void) abort(header.error());
            }
            co_return std::unexpected(header.error());
        }
        request_data_frame_active_ = true;
        request_data_frame_remaining_ = len;
        request_data_frame_end_ = end_stream;
    }

    auto written = co_await stream_.write(buf, len, end_stream, remaining_timeout(deadline));
    if (!written || *written == 0) {
        const common::IoErr error = written ? common::IoErr::WouldBlock : written.error();
        (void) abort(error);
        co_return std::unexpected(error);
    }
    request_data_frame_remaining_ -= *written;
    request_body_sent_ += *written;
    if (request_data_frame_remaining_ == 0) {
        request_data_frame_active_ = false;
        request_data_frame_end_ = false;
        if (end_stream) {
            request_finished_ = true;
        }
    }
    co_return *written;
}

async::Task<common::IoResult<void>> ClientHttp3Request::write_trailer(const HttpHeaders &headers,
                                                                      std::chrono::milliseconds timeout) noexcept {
    if (terminal_error_ != common::IoErr::None) {
        co_return std::unexpected(terminal_error_);
    }
    if (conn_ == nullptr || !request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (writing_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    BoolOperationGuard guard(writing_);
    if (request_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (request_data_frame_active_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    if (request_content_length_seen_ && request_body_sent_ != request_content_length_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    Http3QpackEncoderIoBufWriter writer(node_pool(),
                                        Http3QpackEncoder::Options{.max_string_size = conn_->max_qpack_string_size()},
                                        512, kHttp3FrameHeaderReserve);
    const std::uint64_t peer_field_limit =
            conn_->peer_settings_received() ? conn_->peer_settings().max_field_section_size : 0;
    std::uint64_t field_section_size = 0;
    for (const auto &field: headers) {
        std::string_view name = field.lowcase_view();
        if (name.empty()) {
            name = field.name_view();
        }
        const std::string_view value = field.value_view();
        if (!is_valid_header_name(name) || !is_valid_header_value(value) || name.front() == ':' ||
            is_forbidden_connection_header(name) || name == "content-length" || name == "te") {
            writer.abort();
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (!account_outgoing_field(field_section_size, name, value, peer_field_limit)) {
            writer.abort();
            co_return std::unexpected(common::IoErr::MessageTooLarge);
        }
        common::IoErr err = writer.encode_field(name, field.name_hash, value);
        if (err != common::IoErr::None) {
            writer.abort();
            co_return std::unexpected(err);
        }
    }

    mem::IoBufChain frame(node_pool());
    auto finished = http3_finish_headers_frame(writer, frame, true);
    if (!finished) {
        co_return std::unexpected(finished.error());
    }
    auto written = co_await write_frame(frame, timeout);
    if (!written) {
        (void) abort(written.error());
        co_return written;
    }
    request_finished_ = true;
    co_return common::IoResult<void>{};
}

Http3ParseStatus ClientHttp3Request::parse_frame_header_once() noexcept {
    const std::size_t before = inbound_buf_.readable_bytes();
    Http3ParseStatus status = frame_parser_.parse(inbound_buf_);
    if (before != inbound_buf_.readable_bytes()) {
        frame_header_in_progress_ = true;
    }
    if (status == Http3ParseStatus::Done) {
        current_frame_ = frame_parser_.header();
        frame_parser_.reset();
        frame_header_in_progress_ = false;
    }
    return status;
}

async::Task<common::IoResult<void>> ClientHttp3Request::read_more_input(std::chrono::milliseconds timeout) noexcept {
    if (inbound_buf_.complete()) {
        co_return common::IoResult<void>{};
    }
    auto read = co_await stream_.read(kHttp3ReadChunkSize, inbound_buf_, timeout);
    if (!read) {
        handle_io_error(read.error());
        co_return std::unexpected(read.error());
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> ClientHttp3Request::skip_frame_payload(std::uint64_t payload_length,
                                                                           std::chrono::milliseconds timeout) noexcept {
    std::uint64_t remaining = payload_length;
    while (remaining != 0) {
        const std::uint64_t available = inbound_buf_.readable_bytes();
        if (available == 0) {
            if (inbound_buf_.complete() && inbound_buf_.readable_bytes() == 0) {
                co_return std::unexpected(fail_response(Http3ErrorCode::RequestIncomplete));
            }
            auto read = co_await read_more_input(timeout);
            if (!read) {
                co_return read;
            }
            continue;
        }
        const std::uint64_t take = std::min(available, remaining);
        inbound_buf_.consume_and_compact(static_cast<std::size_t>(take));
        remaining -= take;
    }
    co_return common::IoResult<void>{};
}

common::IoErr ClientHttp3Request::begin_header_block(bool trailer) noexcept {
    if (current_head_ != nullptr || pool_ == nullptr) {
        return fail_response(Http3ErrorCode::InternalError);
    }
    void *storage = pool_->alloc(sizeof(Http3ResponseHead), alignof(Http3ResponseHead));
    if (storage == nullptr) {
        return fail_response(Http3ErrorCode::InternalError, common::IoErr::NoMem);
    }
    current_head_ = new (storage) Http3ResponseHead(*pool_);
    current_block_trailer_ = trailer;
    current_block_has_status_ = false;
    saw_regular_header_ = false;
    current_content_length_seen_ = false;
    current_field_section_size_ = 0;
    pending_name_ = {};
    pending_name_hash_ = 0;
    pending_name_stable_ = false;
    response_parse_error_ = Http3ErrorCode::GeneralProtocolError;
    qpack_decoder_.begin_block(this, &decoder_ops());
    return common::IoErr::None;
}

async::Task<common::IoResult<void>> ClientHttp3Request::parse_header_block(bool trailer, std::uint64_t payload_length,
                                                                           std::chrono::milliseconds timeout) noexcept {
    common::IoErr err = begin_header_block(trailer);
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    std::uint64_t remaining = payload_length;
    if (remaining == 0) {
        err = qpack_decoder_.decode(nullptr, 0, true);
    }
    while (err == common::IoErr::None && remaining != 0) {
        mem::IoBuf *buf = inbound_buf_.first_readable();
        if (buf == nullptr || buf->readable() == 0) {
            if (inbound_buf_.complete() && inbound_buf_.readable_bytes() == 0) {
                err = fail_response(Http3ErrorCode::RequestIncomplete);
                break;
            }
            auto read = co_await read_more_input(timeout);
            if (!read) {
                co_return read;
            }
            continue;
        }
        const std::size_t take = std::min<std::uint64_t>(buf->readable(), remaining);
        err = qpack_decoder_.decode(buf->readable_data(), take, static_cast<std::uint64_t>(take) == remaining);
        inbound_buf_.consume_and_compact(take);
        remaining -= take;
    }
    if (err != common::IoErr::None) {
        if (response_parse_error_ == Http3ErrorCode::GeneralProtocolError) {
            (void) fail_response(Http3ErrorCode::QpackDecompressionFailed, err);
        }
        co_return std::unexpected(err);
    }
    err = complete_header_block();
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    co_return common::IoResult<void>{};
}

common::IoErr ClientHttp3Request::complete_header_block() noexcept {
    if (current_head_ == nullptr || pending_name_.data() != nullptr) {
        return fail_response(Http3ErrorCode::MessageError);
    }
    if (current_block_trailer_) {
        if (current_block_has_status_) {
            return fail_response(Http3ErrorCode::MessageError);
        }
        current_head_->kind = OutgoingHeaderKind::Trailer;
        current_head_->status_code = 0;
        trailer_received_ = true;
    } else {
        if (!current_block_has_status_ || current_head_->status_code < 100 || current_head_->status_code > 999 ||
            current_head_->status_code == 101) {
            return fail_response(Http3ErrorCode::MessageError);
        }
        current_head_->kind = is_informational(current_head_->status_code) ? OutgoingHeaderKind::Informational
                                                                           : OutgoingHeaderKind::Final;
        if (current_head_->kind == OutgoingHeaderKind::Final) {
            final_head_received_ = true;
            response_content_length_seen_ = current_content_length_seen_;
            response_no_body_ = response_must_not_have_body(request_method_, current_head_->status_code);
            if (current_head_->status_code == 204 && response_content_length_seen_) {
                return fail_response(Http3ErrorCode::MessageError);
            }
        }
    }
    pending_head_ = current_head_;
    current_head_ = nullptr;
    pending_name_ = {};
    return common::IoErr::None;
}

const Http3QpackDecoder::Ops &ClientHttp3Request::decoder_ops() noexcept {
    static const Http3QpackDecoder::Ops kOps{
            &ClientHttp3Request::on_indexed_field, &ClientHttp3Request::on_indexed_name,
            &ClientHttp3Request::on_name_raw,      &ClientHttp3Request::on_name_huffman,
            &ClientHttp3Request::on_value_raw,     &ClientHttp3Request::on_value_huffman,
    };
    return kOps;
}

common::IoErr ClientHttp3Request::on_indexed_field(void *owner, Http3QpackDecoder::TableEntryView entry) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    return request == nullptr ? common::IoErr::Invalid
                              : request->commit_field(entry.name, entry.name_hash, entry.value, true);
}

common::IoErr ClientHttp3Request::on_indexed_name(void *owner, std::string_view name,
                                                  std::uint64_t name_hash) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    if (request == nullptr) {
        return common::IoErr::Invalid;
    }
    request->pending_name_ = name;
    request->pending_name_hash_ = name_hash;
    request->pending_name_stable_ = true;
    return common::IoErr::None;
}

common::IoErr ClientHttp3Request::on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    if (request == nullptr || request->pool_ == nullptr) {
        return common::IoErr::Invalid;
    }
    std::string_view name;
    std::uint64_t hash = 0;
    common::IoErr err = detail::materialize_name_raw(*request->pool_, data, len, name, hash);
    if (err == common::IoErr::None) {
        request->pending_name_ = name;
        request->pending_name_hash_ = hash;
        request->pending_name_stable_ = true;
    }
    return err;
}

common::IoErr ClientHttp3Request::on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    if (request == nullptr || request->pool_ == nullptr) {
        return common::IoErr::Invalid;
    }
    std::string_view name;
    std::uint64_t hash = 0;
    common::IoErr err = detail::materialize_name_huffman(*request->pool_, data, len, name, hash);
    if (err == common::IoErr::None) {
        request->pending_name_ = name;
        request->pending_name_hash_ = hash;
        request->pending_name_stable_ = true;
    }
    return err;
}

common::IoErr ClientHttp3Request::on_value_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    if (request == nullptr || request->pool_ == nullptr || request->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    std::string_view value;
    common::IoErr err = detail::materialize_value_raw(*request->pool_, data, len, value);
    if (err != common::IoErr::None) {
        return err;
    }
    std::string_view name = request->pending_name_;
    const std::uint64_t hash = request->pending_name_hash_;
    const bool stable = request->pending_name_stable_;
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_stable_ = false;
    return request->commit_field(name, hash, value, stable);
}

common::IoErr ClientHttp3Request::on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    if (request == nullptr || request->pool_ == nullptr || request->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    std::string_view value;
    common::IoErr err = detail::materialize_value_huffman(*request->pool_, data, len, value);
    if (err != common::IoErr::None) {
        return err;
    }
    std::string_view name = request->pending_name_;
    const std::uint64_t hash = request->pending_name_hash_;
    const bool stable = request->pending_name_stable_;
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_stable_ = false;
    return request->commit_field(name, hash, value, stable);
}

common::IoErr ClientHttp3Request::commit_field(std::string_view name, std::uint64_t name_hash, std::string_view value,
                                               bool stable) noexcept {
    if (current_head_ == nullptr || name.empty() || !is_valid_header_value(value)) {
        return fail_response(Http3ErrorCode::MessageError);
    }
    common::IoErr accounted = account_field(name, value);
    if (accounted != common::IoErr::None) {
        return accounted;
    }
    if (name.front() == ':') {
        if (name != ":status" || saw_regular_header_ || current_block_trailer_) {
            return fail_response(Http3ErrorCode::MessageError);
        }
        return handle_status(value);
    }
    return commit_regular_header(name, name_hash, value, stable);
}

common::IoErr ClientHttp3Request::commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                        std::string_view value, bool stable) noexcept {
    if (!is_valid_header_name(name) || is_forbidden_connection_header(name) || name == "te") {
        return fail_response(Http3ErrorCode::MessageError);
    }
    if (name == "content-length") {
        common::IoErr err = handle_content_length(value);
        if (err != common::IoErr::None) {
            return err;
        }
    }

    std::string_view stored_name = stable ? name : copy_to_pool(name);
    if (!stored_name.data() && !name.empty()) {
        return fail_response(Http3ErrorCode::InternalError, common::IoErr::NoMem);
    }
    if (current_head_->headers.add_view(stored_name, value, stored_name.data(), name_hash) == nullptr) {
        return fail_response(Http3ErrorCode::InternalError, common::IoErr::NoMem);
    }
    saw_regular_header_ = true;
    return common::IoErr::None;
}

common::IoErr ClientHttp3Request::handle_status(std::string_view value) noexcept {
    if (current_block_has_status_ || value.size() != 3) {
        return fail_response(Http3ErrorCode::MessageError);
    }
    int status = 0;
    for (char ch: value) {
        if (ch < '0' || ch > '9') {
            return fail_response(Http3ErrorCode::MessageError);
        }
        status = status * 10 + (ch - '0');
    }
    current_head_->status_code = status;
    current_block_has_status_ = true;
    return common::IoErr::None;
}

common::IoErr ClientHttp3Request::handle_content_length(std::string_view value) noexcept {
    if (current_block_trailer_) {
        return fail_response(Http3ErrorCode::MessageError);
    }
    auto parsed = parse_content_length(value);
    if (!parsed || (current_content_length_seen_ && expected_content_length_ != *parsed)) {
        return fail_response(Http3ErrorCode::MessageError);
    }
    current_content_length_seen_ = true;
    expected_content_length_ = *parsed;
    return common::IoErr::None;
}

common::IoErr ClientHttp3Request::account_field(std::string_view name, std::string_view value) noexcept {
    constexpr std::size_t kFieldOverhead = 32;
    const std::size_t max_size = conn_ == nullptr ? 0 : conn_->max_field_section_size();
    if (value.size() > std::numeric_limits<std::size_t>::max() - kFieldOverhead ||
        name.size() > std::numeric_limits<std::size_t>::max() - kFieldOverhead - value.size()) {
        return fail_response(Http3ErrorCode::ExcessiveLoad);
    }
    const std::size_t field_size = name.size() + value.size() + kFieldOverhead;
    if (current_field_section_size_ > std::numeric_limits<std::size_t>::max() - field_size ||
        (max_size != 0 && current_field_section_size_ + field_size > max_size)) {
        return fail_response(Http3ErrorCode::ExcessiveLoad);
    }
    current_field_section_size_ += field_size;
    return common::IoErr::None;
}

std::string_view ClientHttp3Request::copy_to_pool(std::string_view value) noexcept {
    return pool_ == nullptr ? std::string_view{} : detail::copy_to_pool(*pool_, value);
}

async::Task<common::IoResult<const Http3ResponseHead *>>
ClientHttp3Request::read_header(std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (reading_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    BoolOperationGuard guard(reading_);
    if (pending_head_ != nullptr) {
        const Http3ResponseHead *head = pending_head_;
        pending_head_ = nullptr;
        co_return head;
    }
    if (recv_state_ == RecvState::Complete) {
        co_return static_cast<const Http3ResponseHead *>(nullptr);
    }
    if (recv_state_ == RecvState::Error) {
        co_return std::unexpected(terminal_error_ == common::IoErr::None ? common::IoErr::Canceled : terminal_error_);
    }
    if (recv_state_ == RecvState::DataPayload) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    for (;;) {
        if (recv_state_ == RecvState::WaitFin) {
            if (inbound_buf_.complete() && inbound_buf_.readable_bytes() == 0) {
                auto finished = finish_response();
                if (!finished) {
                    co_return std::unexpected(finished.error());
                }
                if (pending_head_ != nullptr) {
                    const Http3ResponseHead *head = pending_head_;
                    pending_head_ = nullptr;
                    co_return head;
                }
                co_return static_cast<const Http3ResponseHead *>(nullptr);
            }
            auto read = co_await read_more_input(timeout);
            if (!read) {
                co_return std::unexpected(read.error());
            }
            if (inbound_buf_.readable_bytes() != 0) {
                co_return std::unexpected(fail_response(Http3ErrorCode::FrameUnexpected));
            }
            continue;
        }

        Http3ParseStatus status = parse_frame_header_once();
        if (status == Http3ParseStatus::NeedMore) {
            if (inbound_buf_.complete() && inbound_buf_.readable_bytes() == 0) {
                if (frame_header_in_progress_ || !final_head_received_) {
                    co_return std::unexpected(fail_response(Http3ErrorCode::RequestIncomplete));
                }
                auto finished = finish_response();
                if (!finished) {
                    co_return std::unexpected(finished.error());
                }
                co_return static_cast<const Http3ResponseHead *>(nullptr);
            }
            auto read = co_await read_more_input(timeout);
            if (!read) {
                co_return std::unexpected(read.error());
            }
            continue;
        }
        if (status == Http3ParseStatus::Error) {
            co_return std::unexpected(fail_response(frame_parser_.error().h3_error, frame_parser_.error().io_error));
        }

        const Http3FrameHeader header = current_frame_;
        if (header.type == static_cast<std::uint64_t>(Http3FrameType::Headers)) {
            const bool trailer = final_head_received_;
            auto parsed = co_await parse_header_block(trailer, header.length, timeout);
            if (!parsed) {
                stream_.close(error_value(response_parse_error_));
                co_return std::unexpected(parsed.error());
            }
            if (pending_head_ == nullptr) {
                co_return std::unexpected(fail_response(Http3ErrorCode::InternalError));
            }
            if (inbound_buf_.complete() && inbound_buf_.readable_bytes() == 0) {
                if (pending_head_->kind == OutgoingHeaderKind::Informational) {
                    co_return std::unexpected(fail_response(Http3ErrorCode::MessageError));
                }
                pending_head_->end_stream = true;
                auto finished = finish_response();
                if (!finished) {
                    co_return std::unexpected(finished.error());
                }
            } else if (trailer) {
                recv_state_ = RecvState::WaitFin;
            }
            const Http3ResponseHead *head = pending_head_;
            pending_head_ = nullptr;
            co_return head;
        }
        if (header.type == static_cast<std::uint64_t>(Http3FrameType::Data)) {
            if (!final_head_received_ || trailer_received_) {
                co_return std::unexpected(fail_response(Http3ErrorCode::FrameUnexpected));
            }
            frame_payload_remaining_ = header.length;
            recv_state_ = RecvState::DataPayload;
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (is_forbidden_request_stream_frame(header.type)) {
            co_return std::unexpected(
                    fail_response(header.type == static_cast<std::uint64_t>(Http3FrameType::PushPromise)
                                          ? Http3ErrorCode::IdError
                                          : Http3ErrorCode::FrameUnexpected));
        }
        auto skipped = co_await skip_frame_payload(header.length, timeout);
        if (!skipped) {
            co_return std::unexpected(skipped.error());
        }
    }
}

async::Task<common::IoResult<mem::IoBufChain>>
ClientHttp3Request::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    mem::IoBufChain out(node_pool());
    if (conn_ == nullptr || !final_head_received_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (reading_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    BoolOperationGuard guard(reading_);
    if (recv_state_ == RecvState::Complete) {
        out.mark_complete();
        co_return out;
    }
    if (recv_state_ == RecvState::Error) {
        co_return std::unexpected(terminal_error_ == common::IoErr::None ? common::IoErr::Canceled : terminal_error_);
    }
    if (max_bytes == 0) {
        co_return out;
    }

    for (;;) {
        if (recv_state_ == RecvState::DataPayload) {
            const std::size_t available = inbound_buf_.readable_bytes();
            if (available != 0 && frame_payload_remaining_ != 0) {
                const std::size_t take = std::min<std::uint64_t>(
                        std::min<std::size_t>(available, max_bytes - out.readable_bytes()), frame_payload_remaining_);
                if (response_no_body_ && take != 0) {
                    co_return std::unexpected(fail_response(Http3ErrorCode::MessageError));
                }
                if (!inbound_buf_.take_prefix(take, out)) {
                    co_return std::unexpected(fail_response(Http3ErrorCode::InternalError, common::IoErr::NoMem));
                }
                frame_payload_remaining_ -= take;
                response_body_received_ += take;
                if (response_content_length_seen_ && response_body_received_ > expected_content_length_) {
                    co_return std::unexpected(fail_response(Http3ErrorCode::MessageError));
                }
                if (out.readable_bytes() == max_bytes) {
                    co_return out;
                }
            }
            if (frame_payload_remaining_ == 0) {
                recv_state_ = RecvState::FrameHeader;
                continue;
            }
            if (inbound_buf_.complete()) {
                co_return std::unexpected(fail_response(Http3ErrorCode::RequestIncomplete));
            }
            auto read = co_await read_more_input(timeout);
            if (!read) {
                co_return std::unexpected(read.error());
            }
            continue;
        }

        if (recv_state_ == RecvState::WaitFin) {
            if (inbound_buf_.readable_bytes() != 0) {
                co_return std::unexpected(fail_response(Http3ErrorCode::FrameUnexpected));
            }
            if (!inbound_buf_.complete()) {
                auto read = co_await read_more_input(timeout);
                if (!read) {
                    co_return std::unexpected(read.error());
                }
                continue;
            }
            auto finished = finish_response();
            if (!finished) {
                co_return std::unexpected(finished.error());
            }
            out.mark_complete();
            co_return out;
        }

        Http3ParseStatus status = parse_frame_header_once();
        if (status == Http3ParseStatus::NeedMore) {
            if (inbound_buf_.complete()) {
                if (frame_header_in_progress_) {
                    co_return std::unexpected(fail_response(Http3ErrorCode::RequestIncomplete));
                }
                auto finished = finish_response();
                if (!finished) {
                    co_return std::unexpected(finished.error());
                }
                out.mark_complete();
                co_return out;
            }
            auto read = co_await read_more_input(timeout);
            if (!read) {
                co_return std::unexpected(read.error());
            }
            continue;
        }
        if (status == Http3ParseStatus::Error) {
            co_return std::unexpected(fail_response(frame_parser_.error().h3_error, frame_parser_.error().io_error));
        }

        const Http3FrameHeader header = current_frame_;
        if (header.type == static_cast<std::uint64_t>(Http3FrameType::Data)) {
            if (trailer_received_) {
                co_return std::unexpected(fail_response(Http3ErrorCode::FrameUnexpected));
            }
            frame_payload_remaining_ = header.length;
            recv_state_ = RecvState::DataPayload;
            continue;
        }
        if (header.type == static_cast<std::uint64_t>(Http3FrameType::Headers)) {
            if (trailer_received_) {
                co_return std::unexpected(fail_response(Http3ErrorCode::FrameUnexpected));
            }
            auto parsed = co_await parse_header_block(true, header.length, timeout);
            if (!parsed) {
                stream_.close(error_value(response_parse_error_));
                co_return std::unexpected(parsed.error());
            }
            if (inbound_buf_.complete() && inbound_buf_.readable_bytes() == 0) {
                pending_head_->end_stream = true;
                auto finished = finish_response();
                if (!finished) {
                    co_return std::unexpected(finished.error());
                }
                out.mark_complete();
                co_return out;
            }
            recv_state_ = RecvState::WaitFin;
            continue;
        }
        if (is_forbidden_request_stream_frame(header.type)) {
            co_return std::unexpected(
                    fail_response(header.type == static_cast<std::uint64_t>(Http3FrameType::PushPromise)
                                          ? Http3ErrorCode::IdError
                                          : Http3ErrorCode::FrameUnexpected));
        }
        auto skipped = co_await skip_frame_payload(header.length, timeout);
        if (!skipped) {
            co_return std::unexpected(skipped.error());
        }
    }
}

common::IoErr ClientHttp3Request::fail_response(Http3ErrorCode error, common::IoErr reason) noexcept {
    response_parse_error_ = error;
    terminal_error_ = reason == common::IoErr::None ? common::IoErr::Invalid : reason;
    recv_state_ = RecvState::Error;
    if (outcome_ != Http3RequestOutcome::Rejected && outcome_ != Http3RequestOutcome::PossiblyProcessed) {
        outcome_ = request_headers_sent_ ? Http3RequestOutcome::PossiblyProcessed : Http3RequestOutcome::NotSent;
    }
    finish_observation();
    stream_.close(error_value(error));
    return terminal_error_;
}

common::IoResult<void> ClientHttp3Request::finish_response() noexcept {
    if (!final_head_received_ ||
        (!response_no_body_ && response_content_length_seen_ && response_body_received_ != expected_content_length_)) {
        return std::unexpected(fail_response(Http3ErrorCode::MessageError));
    }
    recv_state_ = RecvState::Complete;
    outcome_ = Http3RequestOutcome::Complete;
    terminal_error_ = common::IoErr::None;
    finish_observation();
    return {};
}

void ClientHttp3Request::handle_io_error(common::IoErr error) noexcept {
    terminal_error_ = error;
    recv_state_ = RecvState::Error;
    if (stream_.reset_received() && stream_.reset_error_code() == error_value(Http3ErrorCode::RequestRejected)) {
        outcome_ = Http3RequestOutcome::Rejected;
    } else if (outcome_ != Http3RequestOutcome::Rejected && outcome_ != Http3RequestOutcome::Complete) {
        outcome_ = request_headers_sent_ ? Http3RequestOutcome::PossiblyProcessed : outcome_;
    }
    finish_observation();
}

common::IoResult<void> ClientHttp3Request::abort(common::IoErr reason) noexcept {
    if (recv_state_ == RecvState::Complete) {
        return std::unexpected(common::IoErr::Already);
    }
    terminal_error_ = reason;
    recv_state_ = RecvState::Error;
    if (outcome_ != Http3RequestOutcome::Rejected && outcome_ != Http3RequestOutcome::PossiblyProcessed) {
        outcome_ = request_headers_sent_ ? Http3RequestOutcome::PossiblyProcessed : Http3RequestOutcome::NotSent;
    }
    auto reset = stream_.reset(error_value(Http3ErrorCode::RequestCancelled));
    auto stopped = stream_.stop_read(error_value(Http3ErrorCode::RequestCancelled));
    finish_observation();
    if (!reset && reset.error() != common::IoErr::Canceled) {
        return std::unexpected(reset.error());
    }
    if (!stopped && stopped.error() != common::IoErr::Canceled) {
        return std::unexpected(stopped.error());
    }
    return {};
}

void ClientHttp3Request::finish_observation() noexcept {
    if (conn_ != nullptr && request_entry_.link.linked()) {
        conn_->unregister_client_request(request_entry_);
    }
}

void ClientHttp3Request::on_rejected(void *owner, std::uint64_t goaway_id) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    if (request != nullptr) {
        request->reject_from_goaway(goaway_id);
    }
}

void ClientHttp3Request::on_connection_close(void *owner, Http3ErrorCode error) noexcept {
    auto *request = static_cast<ClientHttp3Request *>(owner);
    if (request != nullptr) {
        request->handle_connection_close(error);
    }
}

void ClientHttp3Request::reject_from_goaway(std::uint64_t) noexcept {
    outcome_ = Http3RequestOutcome::Rejected;
    terminal_error_ = common::IoErr::Canceled;
    recv_state_ = RecvState::Error;
    (void) stream_.reset(error_value(Http3ErrorCode::RequestCancelled));
    (void) stream_.stop_read(error_value(Http3ErrorCode::RequestCancelled));
}

void ClientHttp3Request::handle_connection_close(Http3ErrorCode) noexcept {
    if (outcome_ != Http3RequestOutcome::Complete && outcome_ != Http3RequestOutcome::Rejected &&
        outcome_ != Http3RequestOutcome::PossiblyProcessed) {
        outcome_ = request_headers_sent_ ? Http3RequestOutcome::PossiblyProcessed : Http3RequestOutcome::NotSent;
    }
    terminal_error_ = common::IoErr::Canceled;
    recv_state_ = RecvState::Error;
}

} // namespace fiber::http
