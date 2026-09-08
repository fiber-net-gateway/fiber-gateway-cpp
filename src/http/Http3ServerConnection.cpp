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
