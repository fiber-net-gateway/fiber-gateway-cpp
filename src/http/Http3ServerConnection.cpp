#include "http/Http3ServerConnection.h"
#include <algorithm>
#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <new>
#include <utility>
#include "http/ServerHttp3Request.h"
namespace fiber::http {
Http3ServerConnection::Http3ServerConnection(const quic::QuicConnection::Options &quic_options,
                                             std::shared_ptr<const HttpHandler> handler,
                                             const Http3ServerOptions &options, void *owner, const Ops &ops) noexcept :
    handler_(std::move(handler)), options_(options), owner_(owner), ops_(ops),
    quic_(make_quic_options(quic_options, this)), local_stream_gate_(quic_),
    control_(quic_, local_stream_gate_, make_settings(options), this, control_ops()) {}
Http3ServerConnection::~Http3ServerConnection() {
    FIBER_ASSERT(live_server_requests_ == 0 && server_request_group_.empty());
    FIBER_ASSERT(start_tasks_.empty() && drain_tasks_.empty());
    FIBER_ASSERT(!idle_timer_.is_in_heap());
}
Http3ServerConnection *Http3ServerConnection::create(const quic::QuicConnection::Options &quic_options,
                                                     std::shared_ptr<const HttpHandler> handler,
                                                     const Http3ServerOptions &options, void *owner,
                                                     const Ops &ops) noexcept {
    if (!handler || ops.on_closed == nullptr || quic_options.role != quic::QuicConnectionRole::Server) {
        return nullptr;
    }
    return new (std::nothrow) Http3ServerConnection(quic_options, std::move(handler), options, owner, ops);
}
quic::QuicConnection::Options Http3ServerConnection::make_quic_options(const quic::QuicConnection::Options &base,
                                                                       Http3ServerConnection *owner) noexcept {
    auto options = base;
    options.destroy_owner = owner;
    options.on_destroy = &destroy_connection;
    options.owner = owner;
    options.ops = {.create_stream = &create_peer_stream,
                   .on_peer_stream_attached = &on_peer_stream_attached,
                   .on_state_change = &on_quic_state_change,
                   .on_capacity_change = &on_quic_capacity_change};
    return options;
}
Http3Settings Http3ServerConnection::make_settings(const Http3ServerOptions &options) noexcept {
    auto settings = options.settings;
    settings.enable_connect_protocol = settings.enable_connect_protocol || options.enable_connect_protocol;
    return settings;
}
const Http3ControlStreams::Ops &Http3ServerConnection::control_ops() noexcept {
    static const Http3ControlStreams::Ops ops{
            .on_control_event = [](void *,
                                   const Http3ControlStreamEvent &) noexcept { return Http3ErrorCode::NoError; },
            .on_push_stream = [](void *) noexcept { return Http3ErrorCode::StreamCreationError; },
            .on_error =
                    [](void *owner, Http3ErrorCode error) noexcept {
                        static_cast<Http3ServerConnection *>(owner)->close(error);
                    }};
    return ops;
}
quic::QuicStream::Lease Http3ServerConnection::create_peer_stream(void *owner, std::uint64_t id) noexcept {
    auto &self = *static_cast<Http3ServerConnection *>(owner);
    if (quic::QuicStream::is_bidirectional_stream_id(id)) {
        return ServerHttp3Request::create(id, self, self.options_, self.handler_);
    }
    return Http3ControlStreams::create_stream();
}
void Http3ServerConnection::on_peer_stream_attached(void *owner, quic::QuicStream &stream) noexcept {
    auto &self = *static_cast<Http3ServerConnection *>(owner);
    if (!stream.bidirectional()) {
        self.control_.accept_peer_stream(stream);
        return;
    }
    if (self.closing()) {
        stream.close(static_cast<std::uint64_t>(Http3ErrorCode::RequestRejected));
        return;
    }
    auto *request = ServerHttp3Request::from_stream(stream);
    FIBER_ASSERT(request != nullptr);
    self.next_rejected_request_id_ = std::max(self.next_rejected_request_id_, stream.stream_id() + 4);
    self.server_request_group_.add();
    ++self.live_server_requests_;
    self.update_idle_timer();
    request->start_read_loop(*self.quic_.loop(), self);
}
void Http3ServerConnection::on_quic_state_change(void *owner, quic::QuicConnection &quic) noexcept {
    auto &self = *static_cast<Http3ServerConnection *>(owner);
    self.local_stream_gate_.on_state_change();
    if (!quic.accepting_new_streams()) {
        self.cancel_idle_timer();
    }
}
void Http3ServerConnection::on_quic_capacity_change(void *owner, quic::QuicConnection &) noexcept {
    static_cast<Http3ServerConnection *>(owner)->local_stream_gate_.on_capacity_change();
}
void Http3ServerConnection::start() noexcept {
    if (state_ != Http3ConnectionState::Prepared) {
        return;
    }
    state_ = Http3ConnectionState::Starting;
    start_tasks_.add();
    async::spawn(*quic_.loop(), [this]() { return run_start(); });
    update_idle_timer();
}
async::DetachedTask Http3ServerConnection::run_start() noexcept {
    auto result = co_await control_.start();
    if (!result && result.error() != common::IoErr::Canceled) {
        close(Http3ErrorCode::InternalError);
    } else if (result && state_ == Http3ConnectionState::Starting) {
        state_ = Http3ConnectionState::Running;
    }
    start_tasks_.done();
}
async::Task<void> Http3ServerConnection::wait_started() noexcept { co_await start_tasks_.join(); }
void Http3ServerConnection::end_server_request() noexcept {
    FIBER_ASSERT(live_server_requests_ != 0);
    --live_server_requests_;
    server_request_group_.done();
    update_idle_timer();
}
void Http3ServerConnection::graceful_shutdown() noexcept {
    if (closing()) {
        return;
    }
    const bool unstarted = state_ == Http3ConnectionState::Prepared;
    state_ = Http3ConnectionState::Draining;
    cancel_idle_timer();
    if (unstarted) {
        close();
        return;
    }
    drain_tasks_.add();
    async::spawn(*quic_.loop(), [this]() { return run_graceful_shutdown(); });
}
async::DetachedTask Http3ServerConnection::run_graceful_shutdown() noexcept {
    co_await start_tasks_.join();
    if (state_ == Http3ConnectionState::Draining) {
        if (control_.local_control_stream_available() && co_await control_.send_goaway(next_rejected_request_id_)) {
            co_await server_request_group_.join();
        }
        close();
    }
    drain_tasks_.done();
}
void Http3ServerConnection::close(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closing || state_ == Http3ConnectionState::Closed) {
        return;
    }
    state_ = Http3ConnectionState::Closing;
    close_error_ = error;
    cancel_idle_timer();
    local_stream_gate_.cancel_all(common::IoErr::Canceled);
    control_.stop(error);
    quic_.close_application(static_cast<std::uint64_t>(error));
}
async::Task<void> Http3ServerConnection::join_protocol_tasks() noexcept {
    co_await start_tasks_.join();
    co_await control_.join_readers();
    co_await drain_tasks_.join();
    state_ = Http3ConnectionState::Closed;
}
async::Task<void> Http3ServerConnection::wait_closed() noexcept {
    auto lease = quic_.lease();
    co_await join_protocol_tasks();
}
void Http3ServerConnection::destroy_connection(void *owner, quic::QuicConnection &quic) noexcept {
    auto *self = static_cast<Http3ServerConnection *>(owner);
    FIBER_ASSERT(!self->cleanup_started_);
    self->cleanup_started_ = true;
    self->cancel_idle_timer();
    if (quic.loop() == nullptr) {
        self->ops_.on_closed(self->owner_, *self);
        delete self;
        return;
    }
    async::spawn(*quic.loop(), [self]() -> async::DetachedTask {
        co_await self->join_protocol_tasks();
        self->ops_.on_closed(self->owner_, *self);
        delete self;
    });
}
void Http3ServerConnection::update_idle_timer() noexcept {
    event::EventLoop *loop = quic_.loop();
    if (loop == nullptr || cleanup_started_ || options_.idle_connection_timeout.count() <= 0) {
        return;
    }
    // Serving anything, or already on the way out, means there is nothing to
    // reclaim; a GOAWAY has been sent in the latter case and the drain runs on
    // its own schedule.
    if (active_server_request_count() != 0 || closing() || !quic_.accepting_new_streams()) {
        cancel_idle_timer();
        return;
    }
    // Restart rather than let a stale deadline through: the count reaching zero
    // is what the timeout measures from.
    cancel_idle_timer();
    loop->post_at<Http3ServerConnection, &Http3ServerConnection::idle_timer_, &Http3ServerConnection::on_idle_timeout>(
            loop->now() + options_.idle_connection_timeout, *this);
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
    if (!connection->closing()) {
        connection->graceful_shutdown();
    }
}


} // namespace fiber::http
