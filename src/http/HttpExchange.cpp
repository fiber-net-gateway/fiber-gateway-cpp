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
    AcceptEncoding,
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
        HeaderMap<RequestHeaderRefKind>::Builder builder(6);
        builder.insert("host", RequestHeaderRefKind::Host);
        builder.insert("content-type", RequestHeaderRefKind::ContentType);
        builder.insert("range", RequestHeaderRefKind::Range);
        builder.insert("if-range", RequestHeaderRefKind::IfRange);
        builder.insert("expect", RequestHeaderRefKind::Expect);
        builder.insert("accept-encoding", RequestHeaderRefKind::AcceptEncoding);
        return std::move(builder).build();
    }();
    return refs;
}

} // namespace

HttpExchange::HttpExchange(net::SocketAddress remote_addr) :
    request_headers_(pool_), request_trailers_(pool_), remote_addr_(std::move(remote_addr)) {}

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
    fiber::async::WaitAwaiter(std::chrono::steady_clock::time_point::max(),
                              &ResponseChannelClosedAwaiter::detach_from_exchange, common::IoErr::WouldBlock),
    exchange_(&exchange) {}

HttpExchange::ResponseChannelClosedAwaiter::~ResponseChannelClosedAwaiter() noexcept {
    // A wait that parked, or whose resume is still queued, holds loop state;
    // both are torn down on the awaiter's own loop.
    if (loop() != nullptr) {
        FIBER_ASSERT(loop()->in_loop());
    }
    detach();
}

bool HttpExchange::ResponseChannelClosedAwaiter::await_ready() noexcept {
    FIBER_ASSERT(!completed());
    FIBER_ASSERT(exchange_ != nullptr);

    if (event::EventLoop::current_or_null() == nullptr || exchange_->io_ == nullptr) {
        set_result(common::IoErr::Invalid);
        mark_completed();
        return true;
    }
    if (exchange_->response_channel_waiter_ != nullptr) {
        set_result(common::IoErr::Busy);
        mark_completed();
        return true;
    }
    if (exchange_->io_->response_channel_closed()) {
        set_result(common::IoErr::None);
        mark_completed();
        return true;
    }
    return false;
}

bool HttpExchange::ResponseChannelClosedAwaiter::await_suspend(std::coroutine_handle<> continuation) noexcept {
    FIBER_ASSERT(!completed());
    FIBER_ASSERT(exchange_ != nullptr);
    FIBER_ASSERT(exchange_->io_ != nullptr);

    event::EventLoop *loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    FIBER_ASSERT(loop->in_loop());

    if (exchange_->response_channel_waiter_ != nullptr) {
        set_result(common::IoErr::Busy);
        mark_completed();
        return false;
    }

    // Claim the waiter slot and park before registering: the io may fire the
    // notification synchronously from inside registration, and that completion
    // must find a consistent waiter.
    exchange_->response_channel_waiter_ = this;
    registered_io_ = exchange_->io_;
    begin_wait(continuation, *loop);

    common::IoErr err = registered_io_->set_response_channel_closed_callback(
            &ResponseChannelClosedAwaiter::on_response_channel_closed, this);
    if (err != common::IoErr::None) {
        // Registration never took hold, so nothing can fire. Mark complete and
        // detach by hand rather than complete(), which would post a resume for
        // a handle we are about to resume inline.
        set_result(err);
        mark_completed();
        detach();
        return false;
    }
    return true;
}

common::IoResult<void> HttpExchange::ResponseChannelClosedAwaiter::await_resume() noexcept {
    FIBER_ASSERT(completed());
    FIBER_ASSERT(registered_io_ == nullptr);
    end_wait();
    detach();
    exchange_ = nullptr;
    common::IoErr error = result();
    FIBER_ASSERT(error != common::IoErr::WouldBlock);
    if (error != common::IoErr::None) {
        return std::unexpected(error);
    }
    return {};
}

void HttpExchange::ResponseChannelClosedAwaiter::on_response_channel_closed(void *ctx) noexcept {
    auto *awaiter = static_cast<ResponseChannelClosedAwaiter *>(ctx);
    FIBER_ASSERT(awaiter != nullptr);
    FIBER_ASSERT(awaiter->loop() != nullptr);
    FIBER_ASSERT(awaiter->loop()->in_loop());

    // The io dropped its callback slot before firing; only the exchange slot
    // is left to release, which complete()'s detach handles.
    awaiter->registered_io_ = nullptr;
    awaiter->complete(common::IoErr::None);
}

void HttpExchange::ResponseChannelClosedAwaiter::detach_from_exchange(fiber::async::WaitAwaiter &base) noexcept {
    auto &self = static_cast<ResponseChannelClosedAwaiter &>(base);
    if (self.registered_io_ != nullptr) {
        // Registered and not yet fired: the io still holds the callback.
        FIBER_ASSERT(self.loop() != nullptr);
        FIBER_ASSERT(self.loop()->in_loop());
        common::IoErr err = self.registered_io_->clear_response_channel_closed_callback(
                &ResponseChannelClosedAwaiter::on_response_channel_closed, &self);
        FIBER_ASSERT(err == common::IoErr::None);
        self.registered_io_ = nullptr;
    }
    if (self.exchange_ != nullptr && self.exchange_->response_channel_waiter_ == &self) {
        self.exchange_->response_channel_waiter_ = nullptr;
    }
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
        case RequestHeaderRefKind::AcceptEncoding:
            if (request_header_refs_.accept_encoding == nullptr) {
                request_header_refs_.accept_encoding = &field;
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
