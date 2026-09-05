#include <fiber/http/ClientHttp2Exchange.h>

#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/ClientHttp2Request.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/Http2Connection.h>
#include <fiber/http/Http2LocalStreamGate.h>

namespace fiber::http {

namespace {

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

} // namespace

ClientHttp2Exchange::ClientHttp2Exchange(Http2LocalStreamGate &gate, mem::BufPool &pool) noexcept :
    gate_(&gate), pool_(&pool) {}

ClientHttp2Exchange::ClientHttp2Exchange(Http2ClientConnection &conn, mem::BufPool &pool) noexcept :
    ClientHttp2Exchange(conn.stream_gate(), pool) {}

ClientHttp2Exchange::ClientHttp2Exchange(Http2Stream::Lease stream, mem::BufPool &pool) noexcept :
    pool_(&pool), stream_(std::move(stream)) {}

ClientHttp2Exchange::ClientHttp2Exchange(ClientHttp2Exchange &&other) noexcept :
    gate_(other.gate_), pool_(other.pool_), stream_(std::move(other.stream_)) {
    other.gate_ = nullptr;
    other.pool_ = nullptr;
}

ClientHttp2Exchange &ClientHttp2Exchange::operator=(ClientHttp2Exchange &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    gate_ = other.gate_;
    pool_ = other.pool_;
    stream_ = std::move(other.stream_);
    other.gate_ = nullptr;
    other.pool_ = nullptr;
    return *this;
}

fiber::async::Task<common::IoResult<void>>
ClientHttp2Exchange::send_request_header(const Http2RequestHead &head, bool end_stream,
                                         std::chrono::milliseconds timeout) noexcept {
    const TimePoint deadline = deadline_after(timeout);
    auto request_result = co_await ensure_request_opened(deadline);
    if (!request_result) {
        co_return std::unexpected(request_result.error());
    }
    ClientHttp2Request *req = *request_result;
    FIBER_ASSERT(req != nullptr);
    co_return co_await req->send_request_header(head, end_stream, remaining_timeout(deadline));
}

fiber::async::Task<common::IoResult<size_t>>
ClientHttp2Exchange::write_all(mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write_all(std::move(chunk), timeout);
}

fiber::async::Task<common::IoResult<size_t>>
ClientHttp2Exchange::write_all(const std::uint8_t *buf, std::size_t len, bool end_stream,
                               std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    mem::IoBufChain chunk(req->node_pool());
    if (end_stream) {
        chunk.mark_complete();
    }
    if (len != 0) {
        mem::IoBuf owned = mem::IoBuf::allocate(len);
        if (!owned) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(owned.writable_data(), buf, len);
        owned.commit(len);
        if (!chunk.append(std::move(owned))) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }

    co_return co_await req->write_all(std::move(chunk), timeout);
}

fiber::async::Task<common::IoResult<size_t>> ClientHttp2Exchange::write(mem::IoBufChain &chunk,
                                                                        std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write(chunk, timeout);
}

fiber::async::Task<common::IoResult<size_t>> ClientHttp2Exchange::write(const std::uint8_t *buf, std::size_t len,
                                                                        bool end_stream,
                                                                        std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write(buf, len, end_stream, timeout);
}

fiber::async::Task<common::IoResult<void>>
ClientHttp2Exchange::write_trailer(const HttpHeaders &headers, std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->write_trailer(headers, timeout);
}

fiber::async::Task<common::IoResult<const Http2ResponseHead *>>
ClientHttp2Exchange::read_header(std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->read_header(timeout);
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
ClientHttp2Exchange::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await req->read_body(max_bytes, timeout);
}

common::IoResult<void> ClientHttp2Exchange::abort(common::IoErr reason) noexcept {
    if (!stream_) {
        gate_ = nullptr;
        return std::unexpected(common::IoErr::Invalid);
    }
    const common::IoErr err = stream_->close_rst(Http2ErrorCode::Cancel, reason);
    if (err != common::IoErr::None && err != common::IoErr::Canceled) {
        return std::unexpected(err);
    }
    return {};
}

void ClientHttp2Exchange::cancel(common::IoErr reason) noexcept { (void) abort(reason); }

Http2ExtendedConnectSupport ClientHttp2Exchange::extended_connect_support() const noexcept {
    if (const ClientHttp2Request *req = request()) {
        return req->extended_connect_support();
    }
    if (!gate_ || !gate_->connection().peer_settings_received()) {
        return Http2ExtendedConnectSupport::Unknown;
    }
    return gate_->connection().peer_enable_connect_protocol() ? Http2ExtendedConnectSupport::Enabled
                                                              : Http2ExtendedConnectSupport::Disabled;
}

fiber::async::Task<common::IoResult<ClientHttp2Request *>>
ClientHttp2Exchange::ensure_request_opened(std::chrono::steady_clock::time_point deadline) noexcept {
    if (stream_) {
        ClientHttp2Request *req = request();
        if (!req) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return req;
    }
    if (!gate_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!pool_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::unique_ptr<ClientHttp2Request> pending(ClientHttp2Request::create(gate_->connection(), *pool_));
    if (!pending) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto attach_result = co_await gate_->attach(pending->stream(), remaining_timeout(deadline));
    if (!attach_result) {
        co_return std::unexpected(attach_result.error());
    }
    stream_ = std::move(*attach_result);
    (void) pending.release();
    ClientHttp2Request *req = request();
    if (!req) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return req;
}

ClientHttp2Request *ClientHttp2Exchange::request() noexcept {
    return stream_ ? static_cast<ClientHttp2Request *>(stream_->owner()) : nullptr;
}

const ClientHttp2Request *ClientHttp2Exchange::request() const noexcept {
    return stream_ ? static_cast<const ClientHttp2Request *>(stream_->owner()) : nullptr;
}

} // namespace fiber::http
