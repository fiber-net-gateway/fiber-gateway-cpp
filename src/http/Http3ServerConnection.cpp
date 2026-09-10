#include "http/Http3ServerConnection.h"

#include <new>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include "http/ServerHttp3Request.h"

namespace fiber::http {

Http3ServerConnection::Http3ServerConnection(const quic::QuicConnection::Options &quic_options,
                                             std::shared_ptr<const HttpHandler> handler,
                                             const Http3ServerOptions &options, void *owner, const Ops &ops) noexcept :
    handler_(std::move(handler)), options_(&options), owner_(owner), ops_(&ops),
    quic_(make_quic_options(quic_options, this)), h3_(quic_, make_http3_options(this)) {
    prepared_ = h3_.prepare().has_value();
}

Http3ServerConnection *Http3ServerConnection::create(const quic::QuicConnection::Options &quic_options,
                                                     std::shared_ptr<const HttpHandler> handler,
                                                     const Http3ServerOptions &options, void *owner,
                                                     const Ops &ops) noexcept {
    if (!handler || ops.on_closed == nullptr) {
        return nullptr;
    }
    return new (std::nothrow) Http3ServerConnection(quic_options, std::move(handler), options, owner, ops);
}

quic::QuicConnection::Options Http3ServerConnection::make_quic_options(const quic::QuicConnection::Options &base,
                                                                       Http3ServerConnection *owner) noexcept {
    quic::QuicConnection::Options options = base;
    options.destroy_owner = owner;
    options.on_destroy = &Http3ServerConnection::destroy_connection;
    return options;
}

Http3Connection::Options Http3ServerConnection::make_http3_options(Http3ServerConnection *owner) noexcept {
    Http3Connection::Options options{};
    options.local_settings = owner->options_->settings;
    options.local_settings.enable_connect_protocol =
            options.local_settings.enable_connect_protocol || owner->options_->enable_connect_protocol;
    options.owner = owner;
    options.ops.create_server_request = &Http3ServerConnection::create_server_request;
    options.ops.on_active_request_count_change = &Http3ServerConnection::on_active_request_count_change;
    return options;
}

quic::QuicStream::Lease Http3ServerConnection::create_server_request(void *owner, std::uint64_t stream_id,
                                                                     Http3Connection &conn) noexcept {
    auto *connection = static_cast<Http3ServerConnection *>(owner);
    if (connection == nullptr) {
        return {};
    }
    return ServerHttp3Request::create(stream_id, conn, *connection->options_, connection->handler_);
}

void Http3ServerConnection::start() noexcept {
    if (!prepared_) {
        h3_.close(Http3ErrorCode::InternalError);
        return;
    }
    event::EventLoop *loop = quic_.loop();
    FIBER_ASSERT(loop != nullptr);
    tasks_.add();
    async::spawn(*loop, [this]() -> async::DetachedTask { return run_start(this); });
    // A session that never sends a request has to be reclaimed too, so the
    // clock starts here rather than at the first request's completion.
    update_idle_timer();
}

void Http3ServerConnection::on_active_request_count_change(void *owner, Http3Connection &) noexcept {
    auto *self = static_cast<Http3ServerConnection *>(owner);
    FIBER_ASSERT(self != nullptr);
    self->update_idle_timer();
}

void Http3ServerConnection::update_idle_timer() noexcept {
    event::EventLoop *loop = quic_.loop();
    if (loop == nullptr || cleanup_started_ || options_->idle_connection_timeout.count() <= 0) {
        return;
    }
    // Serving anything, or already on the way out, means there is nothing to
    // reclaim; a GOAWAY has been sent in the latter case and the drain runs on
    // its own schedule.
    if (h3_.active_server_request_count() != 0 || h3_.closing()) {
        cancel_idle_timer();
        return;
    }
    // Restart rather than let a stale deadline through: the count reaching zero
    // is what the timeout measures from.
    cancel_idle_timer();
    loop->post_at<Http3ServerConnection, &Http3ServerConnection::idle_timer_, &Http3ServerConnection::on_idle_timeout>(
            loop->now() + options_->idle_connection_timeout, *this);
}

void Http3ServerConnection::cancel_idle_timer() noexcept {
    event::EventLoop *loop = quic_.loop();
    if (loop != nullptr && idle_timer_.is_in_heap()) {
        loop->cancel<Http3ServerConnection, &Http3ServerConnection::idle_timer_>(*this);
    }
}

void Http3ServerConnection::on_idle_timeout(Http3ServerConnection *connection) noexcept {
    FIBER_ASSERT(connection != nullptr);
    // Retire politely: GOAWAY, then let anything that raced in finish. A request
    // arriving between the timer firing and this running is exactly that race,
    // and graceful_shutdown() waits it out.
    if (!connection->h3_.closing()) {
        connection->graceful_shutdown();
    }
}

async::DetachedTask Http3ServerConnection::run_start(Http3ServerConnection *connection) noexcept {
    if (connection == nullptr) {
        co_return;
    }
    auto started = co_await connection->h3_.start();
    if (!started && started.error() != common::IoErr::Canceled) {
        connection->h3_.close(Http3ErrorCode::InternalError);
    }
    connection->tasks_.done();
    co_return;
}

void Http3ServerConnection::destroy_connection(void *owner, quic::QuicConnection &connection) noexcept {
    auto *self = static_cast<Http3ServerConnection *>(owner);
    if (self == nullptr || self->cleanup_started_) {
        return;
    }

    self->cleanup_started_ = true;
    self->cancel_idle_timer();
    event::EventLoop *loop = connection.loop();
    if (loop == nullptr) {
        self->ops_->on_closed(self->owner_, *self);
        delete self;
        return;
    }
    async::spawn(*loop, [self]() -> async::DetachedTask { return run_cleanup(self); });
}

async::DetachedTask Http3ServerConnection::run_cleanup(Http3ServerConnection *connection) noexcept {
    if (connection == nullptr) {
        co_return;
    }
    co_await connection->tasks_.join();
    co_await connection->h3_.wait_closed();
    // Fires on the connection's own loop, so the owner can unlink it from a
    // registry and release its accounting before the object goes away.
    connection->ops_->on_closed(connection->owner_, *connection);
    delete connection;
    co_return;
}

} // namespace fiber::http
