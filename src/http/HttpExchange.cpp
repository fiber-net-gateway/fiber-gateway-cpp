#include <fiber/http/HttpExchange.h>

#include <cstring>
#include <limits>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/HeaderMap.h>
#include <fiber/http/HttpExchangeIo.h>

namespace fiber::http {

namespace {

enum class RequestHeaderRefKind : std::uint8_t {
    Host,
    ContentType,
    Range,
    IfRange,
    Expect,
};

bool is_terminal_response_write_error(common::IoErr error) noexcept {
    switch (error) {
        case common::IoErr::None:
        case common::IoErr::Invalid:
        case common::IoErr::Busy:
        case common::IoErr::Already:
        case common::IoErr::NoMem:
        case common::IoErr::MessageTooLarge:
        case common::IoErr::NotSupported:
            return false;
        default:
            return true;
    }
}

const HeaderMap<RequestHeaderRefKind> &request_header_ref_map() noexcept {
    static HeaderMap<RequestHeaderRefKind> refs = []() {
        HeaderMap<RequestHeaderRefKind>::Builder builder(5);
        builder.insert("host", RequestHeaderRefKind::Host);
        builder.insert("content-type", RequestHeaderRefKind::ContentType);
        builder.insert("range", RequestHeaderRefKind::Range);
        builder.insert("if-range", RequestHeaderRefKind::IfRange);
        builder.insert("expect", RequestHeaderRefKind::Expect);
        return std::move(builder).build();
    }();
    return refs;
}

} // namespace

HttpExchange::HttpExchange(mem::IoBufNodePool &node_pool, net::SocketAddress remote_addr) :
    header_bufs_(node_pool), trailer_bufs_(node_pool), request_headers_(pool_), request_trailers_(pool_),
    remote_addr_(std::move(remote_addr)) {}

HttpExchange::~HttpExchange() { FIBER_ASSERT(response_channel_waiter_ == nullptr); }

std::string_view HttpExchange::header(std::string_view name) const noexcept { return request_headers_.get(name); }

void HttpExchange::set_io(HttpExchangeIo *io) noexcept {
    if (io != nullptr) {
        FIBER_ASSERT(io_ == nullptr);
        FIBER_ASSERT(response_channel_waiter_ == nullptr);
    } else {
        FIBER_ASSERT(response_channel_waiter_ == nullptr);
    }
    io_ = io;
}

bool HttpExchange::response_channel_closed() const noexcept {
    FIBER_ASSERT(io_ != nullptr);
    return io_ == nullptr || io_->response_channel_closed();
}

HttpExchange::ResponseChannelClosedAwaiter HttpExchange::wait_response_channel_closed() noexcept {
    return ResponseChannelClosedAwaiter(*this);
}

HttpExchange::ResponseChannelClosedAwaiter::ResponseChannelClosedAwaiter(HttpExchange &exchange) noexcept :
    exchange_(&exchange) {}

HttpExchange::ResponseChannelClosedAwaiter::~ResponseChannelClosedAwaiter() noexcept {
    if (resume_entry_.is_in_queue()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        loop_->cancel<ResponseChannelClosedAwaiter, &ResponseChannelClosedAwaiter::resume_entry_>(*this);
    }
    if (registered_io_ != nullptr) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        common::IoErr err = registered_io_->clear_response_channel_closed_callback(
                &ResponseChannelClosedAwaiter::on_response_channel_closed, this);
        FIBER_ASSERT(err == common::IoErr::None);
        registered_io_ = nullptr;
    }
    detach_waiter();
    if (state_ != State::Completed) {
        state_ = State::Abandoned;
    }
}

bool HttpExchange::ResponseChannelClosedAwaiter::await_ready() noexcept {
    FIBER_ASSERT(state_ == State::Created);
    FIBER_ASSERT(exchange_ != nullptr);

    loop_ = event::EventLoop::current_or_null();
    if (loop_ == nullptr || exchange_->io_ == nullptr) {
        make_ready(common::IoErr::Invalid);
        return true;
    }
    if (exchange_->response_channel_waiter_ != nullptr) {
        make_ready(common::IoErr::Busy);
        return true;
    }
    if (exchange_->io_->response_channel_closed()) {
        make_ready(common::IoErr::None);
        return true;
    }
    return false;
}

bool HttpExchange::ResponseChannelClosedAwaiter::await_suspend(std::coroutine_handle<> continuation) noexcept {
    FIBER_ASSERT(state_ == State::Created);
    FIBER_ASSERT(exchange_ != nullptr);
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(exchange_->io_ != nullptr);

    if (exchange_->response_channel_waiter_ != nullptr) {
        make_ready(common::IoErr::Busy);
        return false;
    }

    continuation_ = continuation;
    registered_io_ = exchange_->io_;
    exchange_->response_channel_waiter_ = this;
    state_ = State::Arming;

    common::IoErr err = registered_io_->set_response_channel_closed_callback(
            &ResponseChannelClosedAwaiter::on_response_channel_closed, this);
    if (err != common::IoErr::None) {
        FIBER_ASSERT(state_ == State::Arming);
        registered_io_ = nullptr;
        detach_waiter();
        make_ready(err);
        return false;
    }
    if (state_ == State::Arming) {
        state_ = State::Armed;
    } else {
        FIBER_ASSERT(state_ == State::ResumeQueued);
    }
    return true;
}

common::IoResult<void> HttpExchange::ResponseChannelClosedAwaiter::await_resume() noexcept {
    FIBER_ASSERT(state_ == State::Ready);
    FIBER_ASSERT(completed_);
    FIBER_ASSERT(registered_io_ == nullptr);
    state_ = State::Completed;
    continuation_ = {};
    detach_waiter();
    exchange_ = nullptr;
    if (result_error_ != common::IoErr::None) {
        return std::unexpected(result_error_);
    }
    return {};
}

void HttpExchange::ResponseChannelClosedAwaiter::on_response_channel_closed(void *ctx) noexcept {
    auto *awaiter = static_cast<ResponseChannelClosedAwaiter *>(ctx);
    FIBER_ASSERT(awaiter != nullptr);
    FIBER_ASSERT(awaiter->loop_ != nullptr);
    FIBER_ASSERT(awaiter->loop_->in_loop());
    FIBER_ASSERT(awaiter->state_ == State::Arming || awaiter->state_ == State::Armed);

    awaiter->registered_io_ = nullptr;
    awaiter->state_ = State::ResumeQueued;
    awaiter->loop_->post_local<ResponseChannelClosedAwaiter, &ResponseChannelClosedAwaiter::resume_entry_,
                               &ResponseChannelClosedAwaiter::on_resume>(*awaiter);
}

void HttpExchange::ResponseChannelClosedAwaiter::on_resume(ResponseChannelClosedAwaiter *awaiter) noexcept {
    FIBER_ASSERT(awaiter != nullptr);
    FIBER_ASSERT(awaiter->loop_ != nullptr);
    FIBER_ASSERT(awaiter->loop_->in_loop());
    FIBER_ASSERT(awaiter->state_ == State::ResumeQueued);

    awaiter->make_ready(common::IoErr::None);
    std::coroutine_handle<> continuation = awaiter->continuation_;
    awaiter->continuation_ = {};
    FIBER_ASSERT(continuation);
    continuation.resume();
}

void HttpExchange::ResponseChannelClosedAwaiter::detach_waiter() noexcept {
    if (exchange_ != nullptr && exchange_->response_channel_waiter_ == this) {
        exchange_->response_channel_waiter_ = nullptr;
    }
}

void HttpExchange::ResponseChannelClosedAwaiter::make_ready(common::IoErr error) noexcept {
    result_error_ = error;
    completed_ = true;
    state_ = State::Ready;
}

void HttpExchange::record_io_error(common::IoErr error) noexcept {
    if (response_stats_.terminal_error == common::IoErr::None) {
        response_stats_.terminal_error = error;
    }
}

void HttpExchange::record_response_write_error(common::IoErr error) noexcept {
    record_io_error(error);
    if (response_write_error_ != common::IoErr::None || !is_terminal_response_write_error(error)) {
        return;
    }
    response_write_error_ = error;
    if (io_ != nullptr) {
        (void) io_->abort(*this, error);
    }
}

void HttpExchange::cache_request_header_field(const HttpHeaders::HeaderField &field) noexcept {
    const auto *kind = request_header_ref_map().get(field.lowcase_view(), field.name_hash);
    if (kind == nullptr) {
        return;
    }
    switch (*kind) {
        case RequestHeaderRefKind::Host:
            if (request_header_refs_.host == nullptr) {
                request_header_refs_.host = &field;
            }
            break;
        case RequestHeaderRefKind::ContentType:
            if (request_header_refs_.content_type == nullptr) {
                request_header_refs_.content_type = &field;
            }
            break;
        case RequestHeaderRefKind::Range:
            if (request_header_refs_.range == nullptr) {
                request_header_refs_.range = &field;
            }
            break;
        case RequestHeaderRefKind::IfRange:
            if (request_header_refs_.if_range == nullptr) {
                request_header_refs_.if_range = &field;
            }
            break;
        case RequestHeaderRefKind::Expect:
            if (request_header_refs_.expect == nullptr) {
                request_header_refs_.expect = &field;
            }
            break;
    }
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
HttpExchange::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto result = co_await io_->read_body(*this, max_bytes, timeout);
    if (!result) {
        record_io_error(result.error());
    }
    co_return std::move(result);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::discard_body(std::chrono::milliseconds timeout) noexcept {
    for (;;) {
        auto result = co_await read_body(4096, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->complete()) {
            break;
        }
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_header(const OutgoingHeaderBlockView &header,
                                                                     std::chrono::milliseconds timeout) {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_write_error_ != common::IoErr::None) {
        co_return std::unexpected(response_write_error_);
    }
    auto result = co_await io_->send_header(*this, header, timeout);
    if (!result) {
        record_response_write_error(result.error());
        co_return result;
    }
    if (header.kind == OutgoingHeaderKind::Final) {
        response_stats_.status_code = header.status_code;
        response_stats_.header_sent = true;
        response_stats_.completed = header.end_stream;
    } else if (header.kind == OutgoingHeaderKind::Trailer && header.end_stream) {
        response_stats_.completed = true;
    }
    co_return result;
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_continue_header(std::chrono::milliseconds timeout) {
    co_return co_await send_header(
            {
                    .kind = OutgoingHeaderKind::Informational,
                    .status_code = 100,
                    .headers = nullptr,
                    .end_stream = false,
            },
            timeout);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_informational_header(int status_code,
                                                                                   const HttpHeaders *headers,
                                                                                   std::chrono::milliseconds timeout) {
    co_return co_await send_header(
            {
                    .kind = OutgoingHeaderKind::Informational,
                    .status_code = status_code,
                    .reason = {},
                    .headers = headers,
                    .body = HttpBodySpec::Auto(),
                    .connection_mode = ResponseConnectionMode::Auto,
                    .end_stream = false,
            },
            timeout);
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_all(mem::IoBufChain chunk,
                                                                     std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_write_error_ != common::IoErr::None) {
        co_return std::unexpected(response_write_error_);
    }
    const std::size_t intended = chunk.readable_bytes();
    const bool end = chunk.complete();
    auto result = co_await io_->write_all(*this, std::move(chunk), timeout);
    if (!result) {
        record_response_write_error(result.error());
        co_return result;
    }
    response_stats_.body_bytes_sent =
            *result > std::numeric_limits<std::size_t>::max() - response_stats_.body_bytes_sent
                    ? std::numeric_limits<std::size_t>::max()
                    : response_stats_.body_bytes_sent + *result;
    if (end && *result == intended) {
        response_stats_.completed = true;
    }
    co_return result;
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_all(const uint8_t *buf, size_t len, bool end,
                                                                     std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_write_error_ != common::IoErr::None) {
        co_return std::unexpected(response_write_error_);
    }
    auto result = co_await io_->write_all(*this, buf, len, end, timeout);
    if (!result) {
        record_response_write_error(result.error());
        co_return result;
    }
    response_stats_.body_bytes_sent =
            *result > std::numeric_limits<std::size_t>::max() - response_stats_.body_bytes_sent
                    ? std::numeric_limits<std::size_t>::max()
                    : response_stats_.body_bytes_sent + *result;
    if (end && *result == len) {
        response_stats_.completed = true;
    }
    co_return result;
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write(mem::IoBufChain &chunk,
                                                                 std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_write_error_ != common::IoErr::None) {
        co_return std::unexpected(response_write_error_);
    }
    const std::size_t intended = chunk.readable_bytes();
    const bool end = chunk.complete();
    auto result = co_await io_->write(*this, chunk, timeout);
    if (!result) {
        record_response_write_error(result.error());
        co_return result;
    }
    response_stats_.body_bytes_sent =
            *result > std::numeric_limits<std::size_t>::max() - response_stats_.body_bytes_sent
                    ? std::numeric_limits<std::size_t>::max()
                    : response_stats_.body_bytes_sent + *result;
    if (end && *result == intended && !chunk.complete()) {
        response_stats_.completed = true;
    }
    co_return result;
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write(const uint8_t *buf, size_t len, bool end,
                                                                 std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_write_error_ != common::IoErr::None) {
        co_return std::unexpected(response_write_error_);
    }
    auto result = co_await io_->write(*this, buf, len, end, timeout);
    if (!result) {
        record_response_write_error(result.error());
        co_return result;
    }
    response_stats_.body_bytes_sent =
            *result > std::numeric_limits<std::size_t>::max() - response_stats_.body_bytes_sent
                    ? std::numeric_limits<std::size_t>::max()
                    : response_stats_.body_bytes_sent + *result;
    if (end && *result == len) {
        response_stats_.completed = true;
    }
    co_return result;
}

common::IoResult<void> HttpExchange::abort(common::IoErr reason) noexcept {
    if (!io_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto result = io_->abort(*this, reason);
    record_io_error(result ? reason : result.error());
    return result;
}

} // namespace fiber::http
