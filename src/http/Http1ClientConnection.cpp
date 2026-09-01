#include <fiber/http/Http1ClientConnection.h>

#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/TcpConnector.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TcpStream.h>
#include "http/TlsAlpn.h"

namespace fiber::http {

namespace {

constexpr std::string_view kHttp11Alpn = "http/1.1";

bool supports_http1_alpn(std::string_view alpn) noexcept { return alpn.empty() || alpn == kHttp11Alpn; }

} // namespace

Http1ClientConnection::IoAwaiter::IoAwaiter(Http1ClientConnection &connection, IoAwaiter *&slot, IoTask task) noexcept :
    connection_(connection), slot_(&slot), loop_(connection.active_loop_), task_(std::move(task)),
    task_awaiter_(task_.operator co_await()) {
    FIBER_ASSERT(loop_ != nullptr);
}

Http1ClientConnection::IoAwaiter::~IoAwaiter() {
    if (resume_entry_.is_in_queue()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        loop_->cancel<IoAwaiter, &IoAwaiter::resume_entry_>(*this);
    }
    if (state_ != State::Waiting) {
        return;
    }

    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    state_ = State::Abandoned;
    detach();
    task_ = {};
    connection_.on_io_awaiter_destroyed();
}

bool Http1ClientConnection::IoAwaiter::await_ready() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::Created);
    FIBER_ASSERT(slot_ != nullptr);
    if (*slot_ == nullptr) {
        return false;
    }

    state_ = State::ReadyError;
    result_err_ = common::IoErr::Busy;
    slot_ = nullptr;
    task_ = {};
    return true;
}

std::coroutine_handle<> Http1ClientConnection::IoAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::Created);
    FIBER_ASSERT(slot_ != nullptr);
    FIBER_ASSERT(*slot_ == nullptr);
    state_ = State::Waiting;
    handle_ = handle;
    *slot_ = this;
    return task_awaiter_.await_suspend(handle);
}

common::IoResult<std::size_t> Http1ClientConnection::IoAwaiter::await_resume() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == State::Waiting) {
        state_ = State::Completed;
        detach();
        return task_awaiter_.await_resume();
    }

    FIBER_ASSERT(state_ == State::ReadyError || state_ == State::CanceledReady);
    state_ = State::Completed;
    return std::unexpected(result_err_);
}

void Http1ClientConnection::IoAwaiter::prepare_cancel(common::IoErr reason) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::Waiting);
    state_ = State::CancelPrepared;
    result_err_ = reason == common::IoErr::None ? common::IoErr::Canceled : reason;
    detach();
    task_ = {};
}

void Http1ClientConnection::IoAwaiter::schedule_cancel_resume() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::CancelPrepared);
    state_ = State::ResumeQueued;
    loop_->post_local<IoAwaiter, &IoAwaiter::resume_entry_, &IoAwaiter::on_cancel_resume>(*this);
}

void Http1ClientConnection::IoAwaiter::detach() noexcept {
    if (slot_ != nullptr && *slot_ == this) {
        *slot_ = nullptr;
    }
    slot_ = nullptr;
}

void Http1ClientConnection::IoAwaiter::on_cancel_resume(IoAwaiter *awaiter) noexcept {
    FIBER_ASSERT(awaiter != nullptr);
    FIBER_ASSERT(awaiter->loop_ != nullptr);
    FIBER_ASSERT(awaiter->loop_->in_loop());
    FIBER_ASSERT(awaiter->state_ == State::ResumeQueued);
    awaiter->state_ = State::CanceledReady;
    auto handle = awaiter->handle_;
    awaiter->handle_ = {};
    if (handle) {
        handle.resume();
    }
}

Http1ClientConnectionOptions Http1ClientConnection::normalize_options(Http1ClientConnectionOptions options) noexcept {
    if (options.tls.enabled()) {
        normalize_http1_alpn(options.tls);
    }
    return options;
}

Http1ClientConnection::Http1ClientConnection(event::EventLoop &loop, Http1ClientConnectionOptions options) noexcept :
    loop_(&loop), options_(normalize_options(std::move(options))) {}

Http1ClientConnection::~Http1ClientConnection() {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ != State::Busy);
    FIBER_ASSERT(active_loop_ == nullptr);
    FIBER_ASSERT(reader_ == nullptr);
    FIBER_ASSERT(writer_ == nullptr);
    close();
}

Http1ClientConnection::ConnectStateGuard::~ConnectStateGuard() {
    if (connection_.state_ == State::Connecting) {
        connection_.state_ = State::Init;
    }
}

common::IoErr Http1ClientConnection::begin_connect() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    if (!loop_->in_loop()) {
        return common::IoErr::NotSupported;
    }
    if (state_ != State::Init) {
        return common::IoErr::Busy;
    }
    state_ = State::Connecting;

    return common::IoErr::None;
}

fiber::async::Task<common::IoResult<void>> Http1ClientConnection::connect(std::chrono::milliseconds timeout) noexcept {
    net::HappyEyeballsOptions options;
    options.total_timeout = timeout;
    return connect_impl({}, options, false);
}

fiber::async::Task<common::IoResult<void>> Http1ClientConnection::connect(std::span<const net::SocketAddress> addresses,
                                                                          net::HappyEyeballsOptions options) noexcept {
    return connect_impl(addresses, options, true);
}

fiber::async::Task<common::IoResult<void>>
Http1ClientConnection::connect_impl(std::span<const net::SocketAddress> addresses, net::HappyEyeballsOptions options,
                                    bool multiple_addresses) noexcept {
    common::IoErr begin_error = begin_connect();
    if (begin_error != common::IoErr::None) {
        co_return std::unexpected(begin_error);
    }
    ConnectStateGuard state_guard(*this);

    std::optional<net::TcpStream::ConnectInfant> infant;
    if (multiple_addresses) {
        auto connect_result = co_await net::TcpConnector::connect(*loop_, addresses, options);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error().code);
        }
        infant.emplace(std::move(*connect_result));
    } else {
        auto connect_result = co_await net::TcpStream::connect(*loop_, options_.peer_addr, options.total_timeout);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
        infant.emplace(std::move(*connect_result));
    }

    FIBER_ASSERT(state_ == State::Connecting);
    FIBER_ASSERT(infant.has_value());
    net::SocketAddress connected_peer = infant->peer();

    net::AcceptResult accept(infant->release_fd(), infant->take_peer());
    std::unique_ptr<HttpTransport> transport;
    if (options_.tls.enabled()) {
        auto transport_result = TlsTransport::create(*loop_, std::move(accept), options_.tls, options_.tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);

        auto handshake_result = co_await transport->handshake(options_.tls.handshake_timeout);
        if (!handshake_result) {
            transport->close();
            co_return std::unexpected(handshake_result.error());
        }

        if (!supports_http1_alpn(transport->negotiated_alpn())) {
            transport->close();
            co_return std::unexpected(common::IoErr::NotSupported);
        }
    } else {
        auto transport_result = TcpTransport::create(*loop_, std::move(accept), options_.tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);
    }

    if (!transport || !transport->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    options_.peer_addr = std::move(connected_peer);
    transport_ = std::move(transport);
    state_ = State::ConnectedIdle;
    keepalive_usable_ = true;
    co_return common::IoResult<void>{};
}

void Http1ClientConnection::assert_active_loop() const noexcept {
    FIBER_ASSERT(active_loop_ != nullptr);
    FIBER_ASSERT(active_loop_->in_loop());
}

void Http1ClientConnection::mark_unusable() noexcept {
    keepalive_usable_ = false;
    active_loop_ = nullptr;
    state_ = State::Closed;
}

void Http1ClientConnection::close() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ != State::Busy);
    FIBER_ASSERT(state_ != State::Connecting);
    FIBER_ASSERT(active_loop_ == nullptr);
    FIBER_ASSERT(reader_ == nullptr);
    FIBER_ASSERT(writer_ == nullptr);
    mark_unusable();
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
}

bool Http1ClientConnection::valid() const noexcept { return transport_ && transport_->valid(); }

bool Http1ClientConnection::idle() const noexcept { return state_ == State::ConnectedIdle && valid(); }

bool Http1ClientConnection::busy() const noexcept { return state_ == State::Busy && valid(); }

bool Http1ClientConnection::connected() const noexcept {
    return (state_ == State::ConnectedIdle || state_ == State::Busy) && valid();
}

bool Http1ClientConnection::reusable() const noexcept { return idle() && keepalive_usable_; }

void Http1ClientConnection::record_request_started() noexcept {
    if (request_count_ != std::numeric_limits<std::uint64_t>::max()) {
        ++request_count_;
    }
}

event::EventLoop &Http1ClientConnection::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

bool Http1ClientConnection::acquire_exchange() noexcept {
    if (state_ != State::ConnectedIdle || !valid() || !keepalive_usable_) {
        return false;
    }
    active_loop_ = &event::EventLoop::current();
    state_ = State::Busy;
    return true;
}

bool Http1ClientConnection::exchange_active() const noexcept { return state_ == State::Busy; }

void Http1ClientConnection::release_exchange(bool keepalive) noexcept {
    FIBER_ASSERT(state_ == State::Busy);
    assert_active_loop();
    FIBER_ASSERT(reader_ == nullptr);
    FIBER_ASSERT(writer_ == nullptr);
    active_loop_ = nullptr;
    if (!keepalive || !valid()) {
        mark_unusable();
        return;
    }
    keepalive_usable_ = true;
    state_ = State::ConnectedIdle;
}

void Http1ClientConnection::fail_exchange(common::IoErr reason) noexcept {
    if (state_ != State::Busy) {
        return;
    }
    assert_active_loop();
    fail_active_exchange(reason);
}

void Http1ClientConnection::fail_active_exchange(common::IoErr reason) noexcept {
    FIBER_ASSERT(state_ == State::Busy);
    assert_active_loop();

    IoAwaiter *reader = reader_;
    IoAwaiter *writer = writer_;
    HttpTransport *transport = transport_.get();
    FIBER_ASSERT(transport != nullptr);

    mark_unusable();

    // Destroy leaf I/O tasks first. A cross-loop RWFd waiter therefore queues
    // its owner-loop cancellation before a stolen lease can be returned home.
    if (reader != nullptr) {
        reader->prepare_cancel(reason);
    }
    if (writer != nullptr) {
        writer->prepare_cancel(reason);
    }
    transport->abandon_pending_io();

    // Resume after the failure call returns so cancellation cannot re-enter it.
    if (reader != nullptr) {
        reader->schedule_cancel_resume();
    }
    if (writer != nullptr) {
        writer->schedule_cancel_resume();
    }
}

void Http1ClientConnection::on_io_awaiter_destroyed() noexcept {
    if (state_ == State::Busy) {
        fail_active_exchange(common::IoErr::Canceled);
    }
}

Http1ClientConnection::IoAwaiter Http1ClientConnection::wait_transport_read(IoTask task) noexcept {
    assert_active_loop();
    return IoAwaiter(*this, reader_, std::move(task));
}

Http1ClientConnection::IoAwaiter Http1ClientConnection::wait_transport_write(IoTask task) noexcept {
    assert_active_loop();
    return IoAwaiter(*this, writer_, std::move(task));
}

} // namespace fiber::http
