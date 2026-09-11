#include "http/Http3ClientConnectionImpl.h"
#include <fiber/async/Timeout.h>
#include <fiber/common/Assert.h>
#include <new>
namespace fiber::http {
Http3ClientConnectionImpl::Http3ClientConnectionImpl(const quic::QuicConnection::Options &quic_options,
                                                     const Options &options) noexcept :
    quic_(make_quic_options(quic_options, this)), local_stream_gate_(quic_),
    control_(quic_, local_stream_gate_, options.local_settings, this, control_ops()),
    drain_timeout_(options.drain_timeout), max_qpack_string_size_(options.max_qpack_string_size),
    max_field_section_size_(options.max_field_section_size) {}
Http3ClientConnectionImpl::~Http3ClientConnectionImpl() {
    FIBER_ASSERT(client_requests_.empty() && client_request_group_.empty());
    FIBER_ASSERT(start_tasks_.empty() && drain_tasks_.empty());
}
Http3ClientConnectionImpl *Http3ClientConnectionImpl::create(const quic::QuicConnection::Options &quic_options,
                                                             const Options &options) noexcept {
    if (quic_options.role != quic::QuicConnectionRole::Client) {
        return nullptr;
    }
    return new (std::nothrow) Http3ClientConnectionImpl(quic_options, options);
}
Http3ClientConnection Http3ClientConnectionImpl::make_handle(quic::QuicConnection::Lease lease) noexcept {
    FIBER_ASSERT(lease.get() == &quic_);
    return Http3ClientConnection(std::move(lease), *this);
}
quic::QuicConnection::Options Http3ClientConnectionImpl::make_quic_options(const quic::QuicConnection::Options &base,
                                                                           Http3ClientConnectionImpl *owner) noexcept {
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
quic::QuicStream::Lease Http3ClientConnectionImpl::create_peer_stream(void *, std::uint64_t) noexcept {
    return Http3ControlStreams::create_stream();
}
void Http3ClientConnectionImpl::on_peer_stream_attached(void *owner, quic::QuicStream &stream) noexcept {
    auto &self = *static_cast<Http3ClientConnectionImpl *>(owner);
    if (stream.bidirectional()) {
        self.close(Http3ErrorCode::StreamCreationError);
        return;
    }
    self.control_.accept_peer_stream(stream);
}
void Http3ClientConnectionImpl::on_quic_state_change(void *owner, quic::QuicConnection &) noexcept {
    static_cast<Http3ClientConnectionImpl *>(owner)->local_stream_gate_.on_state_change();
}
void Http3ClientConnectionImpl::on_quic_capacity_change(void *owner, quic::QuicConnection &) noexcept {
    static_cast<Http3ClientConnectionImpl *>(owner)->local_stream_gate_.on_capacity_change();
}
const Http3ControlStreams::Ops &Http3ClientConnectionImpl::control_ops() noexcept {
    static const Http3ControlStreams::Ops ops{
            .on_control_event =
                    [](void *owner, const Http3ControlStreamEvent &event) noexcept {
                        if (event.type == Http3ControlStreamEventType::MaxPushId) {
                            return Http3ErrorCode::FrameUnexpected;
                        }
                        if (event.type == Http3ControlStreamEventType::Goaway &&
                            !static_cast<Http3ClientConnectionImpl *>(owner)->apply_peer_goaway(event.id)) {
                            return Http3ErrorCode::IdError;
                        }
                        return Http3ErrorCode::NoError;
                    },
            .on_push_stream = [](void *) noexcept { return Http3ErrorCode::IdError; },
            .on_error =
                    [](void *owner, Http3ErrorCode error) noexcept {
                        static_cast<Http3ClientConnectionImpl *>(owner)->close(error);
                    }};
    return ops;
}
async::Task<common::IoResult<void>> Http3ClientConnectionImpl::start() noexcept {
    if (state_ != Http3ConnectionState::Prepared) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (quic_.closing()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    state_ = Http3ConnectionState::Starting;
    start_tasks_.add();
    struct Scope {
        Http3ClientConnectionImpl &self;
        bool completed = false;
        ~Scope() {
            if (!completed) {
                self.close(Http3ErrorCode::RequestCancelled);
            }
            self.start_tasks_.done();
        }
    } scope{*this};
    auto result = co_await control_.start();
    if (!result) {
        close(Http3ErrorCode::InternalError);
    } else if (state_ == Http3ConnectionState::Starting) {
        state_ = Http3ConnectionState::Running;
    }
    scope.completed = true;
    co_return result;
}
void Http3ClientConnectionImpl::graceful_shutdown(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closing || state_ == Http3ConnectionState::Closed || !drain_tasks_.empty()) {
        return;
    }
    const bool unstarted = state_ == Http3ConnectionState::Prepared;
    close_error_ = error;
    state_ = Http3ConnectionState::Draining;
    local_stream_gate_.cancel_all(quic::QuicStreamType::Bidirectional, common::IoErr::Canceled);
    if (unstarted) {
        close(error);
        return;
    }
    drain_tasks_.add();
    async::spawn(*quic_.loop(), [this, error]() { return run_graceful_shutdown(error); });
}
async::DetachedTask Http3ClientConnectionImpl::run_graceful_shutdown(Http3ErrorCode error) noexcept {
    co_await start_tasks_.join();
    if (state_ == Http3ConnectionState::Draining) {
        if (control_.local_control_stream_available() && co_await control_.send_goaway(0)) {
            (void) co_await async::timeout_for([this]() { return client_request_group_.join(); }, drain_timeout_);
        }
        close(error);
    }
    drain_tasks_.done();
}
void Http3ClientConnectionImpl::close(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closing || state_ == Http3ConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = Http3ConnectionState::Closing;
    local_stream_gate_.cancel_all(common::IoErr::Canceled);
    detach_client_requests(error);
    control_.stop(error);
    quic_.close_application(static_cast<std::uint64_t>(error));
}
async::Task<void> Http3ClientConnectionImpl::join_protocol_tasks() noexcept {
    co_await start_tasks_.join();
    co_await control_.join_readers();
    co_await drain_tasks_.join();
    state_ = Http3ConnectionState::Closed;
}
async::Task<void> Http3ClientConnectionImpl::wait_closed() noexcept {
    auto lease = quic_.lease();
    co_await join_protocol_tasks();
}
void Http3ClientConnectionImpl::destroy_connection(void *owner, quic::QuicConnection &quic) noexcept {
    auto *self = static_cast<Http3ClientConnectionImpl *>(owner);
    FIBER_ASSERT(!self->cleanup_started_);
    self->cleanup_started_ = true;
    if (quic.loop() == nullptr) {
        delete self;
        return;
    }
    async::spawn(*quic.loop(), [self]() -> async::DetachedTask {
        co_await self->join_protocol_tasks();
        delete self;
    });
}
common::IoResult<void> Http3ClientConnectionImpl::register_client_request(Http3ClientRequestEntry &entry) noexcept {
    if (!accepting_requests() || entry.link.linked() || entry.owner == nullptr || entry.on_rejected == nullptr ||
        entry.on_connection_close == nullptr || entry.stream_id == quic::kQuicUnassignedStreamId ||
        !quic::QuicStream::is_bidirectional_stream_id(entry.stream_id)) {
        return std::unexpected(accepting_requests() ? common::IoErr::Invalid : common::IoErr::Canceled);
    }
    client_requests_.push_back(entry);
    client_request_group_.add();
    return {};
}

void Http3ClientConnectionImpl::unregister_client_request(Http3ClientRequestEntry &entry) noexcept {
    if (!entry.link.linked()) {
        return;
    }
    client_requests_.erase(entry);
    client_request_group_.done();
}

common::IoResult<void> Http3ClientConnectionImpl::apply_peer_goaway(std::uint64_t id) noexcept {
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

void Http3ClientConnectionImpl::reject_client_requests(std::uint64_t goaway_id) noexcept {
    // Requests still queued for a stream id have not been sent, and any id they
    // would get is above the GOAWAY point. QUIC has no idea the peer stopped
    // accepting requests, so its credit alone would keep them waiting.
    local_stream_gate_.cancel_all(quic::QuicStreamType::Bidirectional, common::IoErr::Canceled);
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

void Http3ClientConnectionImpl::detach_client_requests(Http3ErrorCode error) noexcept {
    Http3ClientRequestEntry *entry = client_requests_.front();
    while (entry != nullptr) {
        Http3ClientRequestEntry *next = client_requests_.next_of(*entry);
        client_requests_.erase(*entry);
        client_request_group_.done();
        entry->on_connection_close(entry->owner, error);
        entry = next;
    }
}


} // namespace fiber::http
