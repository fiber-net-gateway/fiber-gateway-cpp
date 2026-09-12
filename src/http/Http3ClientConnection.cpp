#include <fiber/http/Http3ClientConnection.h>

#include <algorithm>
#include <string_view>
#include <utility>

#include <fiber/async/Timeout.h>
#include <fiber/common/Assert.h>
#include <fiber/http/ClientHttp3Exchange.h>
#include <fiber/http/Http3Client.h>

namespace fiber::http {

namespace {

constexpr std::string_view kHttp3Alpn = "h3";
constexpr std::uint64_t kHttp3PeerUnidirectionalStreamLimit = 16;

} // namespace

Http3ClientConnection::Http3ClientConnection(Http3Client &client, const Http3ClientConnectOptions &options) noexcept :
    client_(client), server_name_(options.server_name), verify_name_(options.verify_name),
    remote_addr_(options.remote_addr), handshake_timeout_(options.handshake_timeout),
    allow_insecure_(options.allow_insecure),
    quic_(client.endpoint(), own_quic_options(make_quic_options(client, options, identity_error_), this)),
    local_stream_gate_(quic_),
    control_(quic_, local_stream_gate_, client.options().local_settings, this, control_ops()),
    drain_timeout_(client.options().drain_timeout), max_qpack_string_size_(client.options().max_qpack_string_size),
    max_field_section_size_(client.options().max_field_section_size) {}

Http3ClientConnection::Http3ClientConnection(Http3Client &client, const quic::QuicConnection::Options &quic_options,
                                             const Http3ClientConnectOptions &options) noexcept :
    client_(client), server_name_(options.server_name), verify_name_(options.verify_name),
    remote_addr_(options.remote_addr), handshake_timeout_(options.handshake_timeout),
    allow_insecure_(options.allow_insecure), quic_(client.endpoint(), own_quic_options(quic_options, this)),
    local_stream_gate_(quic_),
    control_(quic_, local_stream_gate_, client.options().local_settings, this, control_ops()),
    drain_timeout_(client.options().drain_timeout), max_qpack_string_size_(client.options().max_qpack_string_size),
    max_field_section_size_(client.options().max_field_section_size) {}

Http3ClientConnection::~Http3ClientConnection() {
    FIBER_ASSERT(!quic_.attached_to_endpoint());
    FIBER_ASSERT(client_requests_.empty() && client_request_group_.empty());
    FIBER_ASSERT(start_tasks_.empty() && drain_tasks_.empty());
}

quic::QuicConnection::Options Http3ClientConnection::make_quic_options(Http3Client &client,
                                                                       const Http3ClientConnectOptions &options,
                                                                       common::IoErr &identity_error) noexcept {
    quic::QuicConnection::Options quic_options{};
    quic_options.role = quic::QuicConnectionRole::Client;
    quic_options.remote_addr = options.remote_addr;
    quic_options.transport = options.transport;
    quic_options.transport.initial_max_streams_bidi = 0;
    quic_options.transport.initial_max_streams_uni =
            std::max(quic_options.transport.initial_max_streams_uni, kHttp3PeerUnidirectionalStreamLimit);
    quic_options.keepalive_interval = options.keepalive_interval;
    quic_options.recv_flow = options.recv_flow;
    quic_options.max_peer_bidirectional_streams = 0;
    quic_options.max_peer_unidirectional_streams = kHttp3PeerUnidirectionalStreamLimit;
    // A client opens no local stream until the peer's transport parameters
    // arrive; HTTP/3 does not attempt 0-RTT, so nothing seeds these earlier.
    quic_options.max_local_bidirectional_streams = 0;
    quic_options.max_local_unidirectional_streams = 0;

    quic::QuicUdpEndpoint &endpoint = client.endpoint();
    FIBER_ASSERT(endpoint.loop().in_loop());
    if (!endpoint.valid()) {
        identity_error = common::IoErr::Invalid;
        return quic_options;
    }
    quic_options.local_addr = endpoint.local_addr();
    auto identity = endpoint.allocate_client_identity();
    if (!identity) {
        identity_error = identity.error();
        return quic_options;
    }
    quic_options.original_destination_connection_id = identity->original_destination_connection_id;
    quic_options.initial_destination_connection_id = identity->original_destination_connection_id;
    quic_options.remote_connection_id = identity->original_destination_connection_id;
    quic_options.local_connection_id = identity->local_connection_id;
    return quic_options;
}

quic::QuicConnection::Options Http3ClientConnection::own_quic_options(const quic::QuicConnection::Options &base,
                                                                      Http3ClientConnection *owner) noexcept {
    auto options = base;
    // Caller-owned storage: no destroy callback, the endpoint counts the
    // connection only while attached.
    options.destroy_owner = nullptr;
    options.on_destroy = nullptr;
    options.owner = owner;
    options.ops = {.create_stream = &create_peer_stream,
                   .on_peer_stream_attached = &on_peer_stream_attached,
                   .on_state_change = &on_quic_state_change,
                   .on_capacity_change = &on_quic_capacity_change,
                   .on_new_tls_session = &on_new_tls_session,
                   .on_new_token = &on_new_token};
    return options;
}

quic::QuicStream::Lease Http3ClientConnection::create_peer_stream(void *, std::uint64_t) noexcept {
    return Http3ControlStreams::create_stream();
}

void Http3ClientConnection::on_peer_stream_attached(void *owner, quic::QuicStream &stream) noexcept {
    auto &self = *static_cast<Http3ClientConnection *>(owner);
    if (stream.bidirectional()) {
        self.close(Http3ErrorCode::StreamCreationError);
        return;
    }
    self.control_.accept_peer_stream(stream);
}

void Http3ClientConnection::on_quic_state_change(void *owner, quic::QuicConnection &) noexcept {
    static_cast<Http3ClientConnection *>(owner)->local_stream_gate_.on_state_change();
}

void Http3ClientConnection::on_quic_capacity_change(void *owner, quic::QuicConnection &) noexcept {
    static_cast<Http3ClientConnection *>(owner)->local_stream_gate_.on_capacity_change();
}

quic::QuicClientCacheKey Http3ClientConnection::cache_key() const noexcept {
    return quic::QuicClientCacheKey{
            .server_name = server_name_,
            .verify_name = verify_name_,
            .remote_addr = remote_addr_,
            .credential = client_.tls_credential(),
            .trust_store = client_.trust_store(),
    };
}

bool Http3ClientConnection::on_new_tls_session(void *owner, quic::QuicConnection &quic, SSL_SESSION *session) noexcept {
    auto &self = *static_cast<Http3ClientConnection *>(owner);
    const quic::QuicClientCacheOps &cache = self.client_.options().cache;
    if (cache.store_session == nullptr) {
        return false;
    }
    return cache.store_session(cache.owner, self.cache_key(), session, quic.peer_transport().params);
}

void Http3ClientConnection::on_new_token(void *owner, quic::QuicConnection &, const std::uint8_t *token,
                                         std::size_t token_len) noexcept {
    auto &self = *static_cast<Http3ClientConnection *>(owner);
    const quic::QuicClientCacheOps &cache = self.client_.options().cache;
    if (cache.store_token == nullptr) {
        return;
    }
    cache.store_token(cache.owner, self.cache_key(), token, token_len);
}

const Http3ControlStreams::Ops &Http3ClientConnection::control_ops() noexcept {
    static const Http3ControlStreams::Ops ops{
            .on_control_event =
                    [](void *owner, const Http3ControlStreamEvent &event) noexcept {
                        if (event.type == Http3ControlStreamEventType::MaxPushId) {
                            return Http3ErrorCode::FrameUnexpected;
                        }
                        if (event.type == Http3ControlStreamEventType::Goaway &&
                            !static_cast<Http3ClientConnection *>(owner)->apply_peer_goaway(event.id)) {
                            return Http3ErrorCode::IdError;
                        }
                        return Http3ErrorCode::NoError;
                    },
            .on_push_stream = [](void *) noexcept { return Http3ErrorCode::IdError; },
            .on_error =
                    [](void *owner, Http3ErrorCode error) noexcept {
                        static_cast<Http3ClientConnection *>(owner)->close(error);
                    }};
    return ops;
}

void Http3ClientConnection::abort_connect(Http3ErrorCode close_code) noexcept {
    // No closing or draining period for a connection that never served a
    // request: the peer has either already closed it or will never hear
    // from it again. One that never attached has nothing on the wire at all,
    // so it is marked Closed in place.
    if (quic_.attached_to_endpoint()) {
        quic_.close_immediately(quic::QuicErrorCode::NoError);
    } else {
        quic_.mark_closed();
    }
    close(close_code);
}

async::Task<Http3ClientConnectResult> Http3ClientConnection::fail_connect(Http3ClientConnectError error,
                                                                          Http3ErrorCode close_code) noexcept {
    abort_connect(close_code);
    co_await wait_closed();
    co_return std::unexpected(error);
}

async::Task<Http3ClientConnectResult> Http3ClientConnection::connect() noexcept {
    quic::QuicUdpEndpoint &endpoint = client_.endpoint();
    FIBER_ASSERT(endpoint.loop().in_loop());
    if (state_ != Http3ConnectionState::Prepared) {
        co_return std::unexpected(Http3ClientConnectError{.phase = Http3ClientConnectPhase::ClientInit,
                                                          .io_error = common::IoErr::Already});
    }
    // Every exit but success ends in abort_connect(): a failure through
    // fail(), which also waits for the teardown; task cancellation -- the
    // frame destroyed at an await -- through this guard, which cannot wait and
    // leaves that to the caller's wait_closed().
    struct Scope {
        Http3ClientConnection &self;
        bool settled = false;
        ~Scope() {
            if (!settled) {
                self.abort_connect(Http3ErrorCode::RequestCancelled);
            }
        }
    } scope{*this};
    auto fail = [&scope, this](Http3ClientConnectError error, Http3ErrorCode close_code) noexcept {
        scope.settled = true;
        return fail_connect(error, close_code);
    };
    auto quic_error = [](quic::QuicConnectError error) noexcept {
        return Http3ClientConnectError{
                .phase = Http3ClientConnectPhase::Quic, .io_error = error.io_error, .quic_error = error};
    };
    if (identity_error_ != common::IoErr::None) {
        co_return co_await fail(quic_error({.phase = quic::QuicConnectPhase::Connection, .io_error = identity_error_}),
                                Http3ErrorCode::InternalError);
    }
    if (!endpoint.running() || remote_addr_.port() == 0 || remote_addr_.ip().is_unspecified() ||
        handshake_timeout_ < std::chrono::milliseconds::zero()) {
        co_return co_await fail(
                quic_error({.phase = quic::QuicConnectPhase::Endpoint, .io_error = common::IoErr::Invalid}),
                Http3ErrorCode::InternalError);
    }

    // The token is validated by QuicConnection::set_initial_token().
    const quic::QuicClientCacheOps &cache = client_.options().cache;
    quic::QuicClientCachedState cached{};
    if (cache.load != nullptr && !cache.load(cache.owner, cache_key(), cached)) {
        cached = {};
    }

    // QUIC requires TLS 1.3; min/max are fixed here rather than caller-configurable.
    quic::QuicClientConnectParams params{};
    params.tls.security = client_.options().tls;
    params.tls.min_version = 0x0304;
    params.tls.max_version = 0x0304;
    params.tls.alpn = client_.alpn();
    params.tls.server_name = server_name_;
    params.tls.verify_name = verify_name_;
    params.allow_insecure = allow_insecure_;
    params.resumption_session = cached.session;
    params.token = cached.token;
    params.token_len = cached.token_len;
    auto connected = quic_.connect(params);
    if (!connected) {
        co_return co_await fail(quic_error(quic_.connect_error(connected.error())), Http3ErrorCode::InternalError);
    }
    auto established = co_await quic_.wait_established(handshake_timeout_);
    if (!established) {
        co_return co_await fail(quic_error(quic_.connect_error(established.error())), Http3ErrorCode::InternalError);
    }
    if (quic_.tls().selected_alpn() != kHttp3Alpn) {
        co_return co_await fail({.phase = Http3ClientConnectPhase::Alpn, .io_error = common::IoErr::NotSupported},
                                Http3ErrorCode::VersionFallback);
    }
    auto started = co_await start();
    if (!started) {
        co_return co_await fail({.phase = Http3ClientConnectPhase::Http3, .io_error = started.error()},
                                Http3ErrorCode::InternalError);
    }
    scope.settled = true;
    co_return Http3ClientConnectResult{};
}

async::Task<common::IoResult<void>> Http3ClientConnection::start() noexcept {
    if (state_ != Http3ConnectionState::Prepared) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (quic_.closing()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    state_ = Http3ConnectionState::Starting;
    start_tasks_.add();
    struct Scope {
        Http3ClientConnection &self;
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

ClientHttp3Exchange Http3ClientConnection::open_exchange(mem::BufPool &pool) noexcept {
    return ClientHttp3Exchange(*this, pool);
}

void Http3ClientConnection::shutdown(Http3ErrorCode error) noexcept { close(error); }

void Http3ClientConnection::graceful_shutdown(Http3ErrorCode error) noexcept {
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
    async::spawn(quic_.loop(), [this, error]() { return run_graceful_shutdown(error); });
}

async::DetachedTask Http3ClientConnection::run_graceful_shutdown(Http3ErrorCode error) noexcept {
    co_await start_tasks_.join();
    if (state_ == Http3ConnectionState::Draining) {
        if (control_.local_control_stream_available() && co_await control_.send_goaway(0)) {
            (void) co_await async::timeout_for([this]() { return client_request_group_.join(); }, drain_timeout_);
        }
        close(error);
    }
    drain_tasks_.done();
}

void Http3ClientConnection::close(Http3ErrorCode error) noexcept {
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

async::Task<void> Http3ClientConnection::join_protocol_tasks() noexcept {
    co_await start_tasks_.join();
    co_await control_.join_readers();
    co_await drain_tasks_.join();
    state_ = Http3ConnectionState::Closed;
}

async::Task<void> Http3ClientConnection::wait_closed() noexcept {
    co_await join_protocol_tasks();
    co_await quic_.wait_closed();
}

common::IoResult<void> Http3ClientConnection::register_client_request(Http3ClientRequestEntry &entry) noexcept {
    if (!accepting_requests() || entry.link.linked() || entry.owner == nullptr || entry.on_rejected == nullptr ||
        entry.on_connection_close == nullptr || entry.stream_id == quic::kQuicUnassignedStreamId ||
        !quic::QuicStream::is_bidirectional_stream_id(entry.stream_id)) {
        return std::unexpected(accepting_requests() ? common::IoErr::Invalid : common::IoErr::Canceled);
    }
    client_requests_.push_back(entry);
    client_request_group_.add();
    return {};
}

void Http3ClientConnection::unregister_client_request(Http3ClientRequestEntry &entry) noexcept {
    if (!entry.link.linked()) {
        return;
    }
    client_requests_.erase(entry);
    client_request_group_.done();
}

common::IoResult<void> Http3ClientConnection::apply_peer_goaway(std::uint64_t id) noexcept {
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

void Http3ClientConnection::reject_client_requests(std::uint64_t goaway_id) noexcept {
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

void Http3ClientConnection::detach_client_requests(Http3ErrorCode error) noexcept {
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
