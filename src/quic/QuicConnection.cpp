#include "QuicConnection.h"

#include <algorithm>
#include <coroutine>
#include <cstring>
#include <expected>
#include <limits>
#include <new>

#include "QuicCrypto.h"
#include "QuicLossRecovery.h"
#include "QuicProtocol.h"
#include "QuicTransportParamsCodec.h"
#include "QuicUdpEndpoint.h"

namespace fiber::quic {

namespace {

constexpr std::uint64_t kStreamTypeMask = 0x02;
constexpr std::uint64_t kStreamInitiatorMask = 0x01;
constexpr std::uint64_t kStreamIncrement = 4;
constexpr QuicEncryptionLevel kCloseLevels[] = {
        QuicEncryptionLevel::Initial,
        QuicEncryptionLevel::Handshake,
        QuicEncryptionLevel::Application,
};

// RFC 9000 doesn't mandate a specific retransmission interval for CONNECTION_CLOSE
// in the Closing state. nginx uses 1s (NGX_QUIC_CC_MIN_INTERVAL); we match that to
// avoid sending more than one CC per second when receiving a burst of peer packets.
constexpr std::chrono::milliseconds kQuicCloseFrameMinInterval{1000};

[[nodiscard]] std::uint64_t stream_sequence(std::uint64_t stream_id) noexcept { return stream_id >> 2; }

[[nodiscard]] std::uint64_t initial_stream_id(QuicConnectionRole role, QuicStreamType type) noexcept {
    std::uint64_t id = role == QuicConnectionRole::Server ? 1 : 0;
    if (type == QuicStreamType::Unidirectional) {
        id |= kStreamTypeMask;
    }
    return id;
}

[[nodiscard]] bool connection_id_equal(const QuicConnectionId &left, const QuicConnectionId &right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    if (left.size() == 0) {
        return true;
    }
    return std::memcmp(left.data(), right.data(), left.size()) == 0;
}

} // namespace

QuicPacketProtectionKeys::QuicPacketProtectionKeys() noexcept { EVP_AEAD_CTX_zero(&aead); }

QuicPacketProtectionKeys::~QuicPacketProtectionKeys() { reset(); }

void QuicPacketProtectionKeys::reset() noexcept {
    if (aead_initialized) {
        EVP_AEAD_CTX_cleanup(&aead);
        aead_initialized = false;
    }
    EVP_AEAD_CTX_zero(&aead);
    key = {};
    iv = {};
    hp = {};
    secret = {};
    secret_len = 0;
    key_len = 0;
    iv_len = 0;
    hp_len = 0;
    hp_key = {};
    hp_chacha20 = false;
    ready = false;
}

void QuicPacketProtectionKeys::swap(QuicPacketProtectionKeys &other) noexcept {
    using std::swap;
    swap(suite, other.suite);
    swap(secret, other.secret);
    swap(key, other.key);
    swap(iv, other.iv);
    swap(hp, other.hp);
    swap(secret_len, other.secret_len);
    swap(key_len, other.key_len);
    swap(iv_len, other.iv_len);
    swap(hp_len, other.hp_len);
    // EVP_AEAD_CTX is a POD value (struct evp_aead_ctx_st). It carries a pointer
    // to the AEAD method plus an inline state union; swapping by value is safe
    // because no internal pointer references the context's own address.
    EVP_AEAD_CTX tmp;
    std::memcpy(&tmp, &aead, sizeof(EVP_AEAD_CTX));
    std::memcpy(&aead, &other.aead, sizeof(EVP_AEAD_CTX));
    std::memcpy(&other.aead, &tmp, sizeof(EVP_AEAD_CTX));
    swap(hp_key, other.hp_key);
    swap(aead_initialized, other.aead_initialized);
    swap(hp_chacha20, other.hp_chacha20);
    swap(ready, other.ready);
}

void QuicCryptoState::reset() noexcept {
    initial_read.reset();
    initial_write.reset();
    early_read.reset();
    early_write.reset();
    handshake_read.reset();
    handshake_write.reset();
    application_read.reset();
    application_write.reset();
    next_application_read.reset();
    next_application_write.reset();
    previous_application_read.reset();
    initial_ready = false;
    next_application_keys_ready = false;
    previous_application_keys_ready = false;
}

class QuicConnection::LocalStreamAttachAwaiter {
public:
    LocalStreamAttachAwaiter(QuicConnection &connection, QuicStreamType type,
                             std::chrono::steady_clock::time_point deadline) noexcept :
        connection_(&connection), type_(type), deadline_(deadline) {}

    LocalStreamAttachAwaiter(const LocalStreamAttachAwaiter &) = delete;
    LocalStreamAttachAwaiter &operator=(const LocalStreamAttachAwaiter &) = delete;
    LocalStreamAttachAwaiter(LocalStreamAttachAwaiter &&) = delete;
    LocalStreamAttachAwaiter &operator=(LocalStreamAttachAwaiter &&) = delete;

    ~LocalStreamAttachAwaiter() {
        cancel_timer();
        if (connection_ != nullptr) {
            connection_->cancel_local_stream_attach_wait(*this);
        }
    }

    bool await_ready() noexcept {
        if (connection_ == nullptr || connection_->local_stream_attach_ready(type_)) {
            return true;
        }
        if (timed_out(std::chrono::steady_clock::now())) {
            result_ = common::IoErr::TimedOut;
            completed_ = true;
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (connection_ == nullptr || connection_->local_stream_attach_ready(type_)) {
            return false;
        }
        loop_ = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(connection_->loop_ == nullptr || connection_->loop_ == loop_);
        if (timed_out(loop_->now())) {
            result_ = common::IoErr::TimedOut;
            completed_ = true;
            loop_ = nullptr;
            return false;
        }

        handle_ = handle;
        connection_->wait_for_local_stream_attach(*this);
        arm_timer();
        return true;
    }

    common::IoErr await_resume() noexcept {
        common::IoErr result = result_;
        cancel_timer();
        if (connection_ != nullptr) {
            connection_->cancel_local_stream_attach_wait(*this);
        }
        connection_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        result_ = common::IoErr::None;
        resume_posted_ = false;
        completed_ = false;
        return result;
    }

    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        cancel_timer();
        post_resume();
    }

private:
    [[nodiscard]] static LocalStreamAttachAwaiter *from_wait_link(common::IntrusiveListHook *hook) noexcept {
        if (hook == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<LocalStreamAttachAwaiter *>(reinterpret_cast<std::uint8_t *>(hook) -
                                                            offsetof(LocalStreamAttachAwaiter, wait_link_));
    }

    [[nodiscard]] bool has_timer() const noexcept { return deadline_ != std::chrono::steady_clock::time_point::max(); }

    [[nodiscard]] bool timed_out(std::chrono::steady_clock::time_point now) const noexcept {
        return has_timer() && now >= deadline_;
    }

    void arm_timer() noexcept {
        if (!has_timer() || loop_ == nullptr) {
            return;
        }
        loop_->post_at<LocalStreamAttachAwaiter, &LocalStreamAttachAwaiter::timer_entry_,
                       &LocalStreamAttachAwaiter::on_timeout>(deadline_, *this);
    }

    void cancel_timer() noexcept {
        if (loop_ != nullptr && timer_entry_.is_in_heap()) {
            loop_->cancel<LocalStreamAttachAwaiter, &LocalStreamAttachAwaiter::timer_entry_>(*this);
        }
    }

    static void on_notify(LocalStreamAttachAwaiter *awaiter) noexcept {
        if (awaiter == nullptr) {
            return;
        }
        awaiter->resume_posted_ = false;
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    static void on_timeout(LocalStreamAttachAwaiter *awaiter) noexcept {
        if (awaiter == nullptr) {
            return;
        }
        awaiter->complete(common::IoErr::TimedOut);
    }

    void post_resume() noexcept {
        if (resume_posted_ || loop_ == nullptr) {
            return;
        }
        resume_posted_ = true;
        loop_->post<LocalStreamAttachAwaiter, &LocalStreamAttachAwaiter::notify_entry_,
                    &LocalStreamAttachAwaiter::on_notify>(*this);
    }

    QuicConnection *connection_ = nullptr;
    QuicStreamType type_ = QuicStreamType::Bidirectional;
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::NotifyEntry notify_entry_{};
    event::EventLoop::TimerEntry timer_entry_{};
    common::IntrusiveListHook wait_link_{};
    common::IoErr result_ = common::IoErr::None;
    bool resume_posted_ = false;
    bool completed_ = false;

    friend class QuicConnection;
};

QuicConnection::Lease::Lease(QuicConnection *connection) noexcept : connection_(connection) {
    if (connection_) {
        connection_->retain();
    }
}

void QuicConnection::Lease::reset() noexcept {
    if (!connection_) {
        return;
    }
    QuicConnection *connection = connection_;
    connection_ = nullptr;
    connection->release();
}

QuicConnection::QuicConnection(const Options &options) noexcept :
    options_(options), next_local_bidi_stream_id_(initial_stream_id(options.role, QuicStreamType::Bidirectional)),
    next_local_uni_stream_id_(initial_stream_id(options.role, QuicStreamType::Unidirectional)) {
    destroy_owner_ = options_.destroy_owner;
    on_destroy_ = options_.on_destroy;
    loop_ = options_.loop != nullptr ? options_.loop : event::EventLoop::current_or_null();
    FIBER_ASSERT(loop_ != nullptr);
    auto peer_stream_limit = [](std::uint64_t concurrent_limit, std::uint64_t transport_limit,
                                std::uint64_t default_limit) noexcept {
        concurrent_limit = std::min(concurrent_limit, kQuicMaxStreamLimit);
        transport_limit = std::min(transport_limit, kQuicMaxStreamLimit);
        return concurrent_limit == default_limit && transport_limit != default_limit ? transport_limit
                                                                                     : concurrent_limit;
    };
    options_.max_peer_bidirectional_streams =
            peer_stream_limit(options_.max_peer_bidirectional_streams, options_.transport.initial_max_streams_bidi,
                              kQuicDefaultMaxBidirectionalStreams);
    options_.max_peer_unidirectional_streams =
            peer_stream_limit(options_.max_peer_unidirectional_streams, options_.transport.initial_max_streams_uni,
                              kQuicDefaultMaxUnidirectionalStreams);
    options_.max_local_bidirectional_streams = std::min(options_.max_local_bidirectional_streams, kQuicMaxStreamLimit);
    options_.max_local_unidirectional_streams =
            std::min(options_.max_local_unidirectional_streams, kQuicMaxStreamLimit);
    if (options_.initial_destination_connection_id.empty()) {
        options_.initial_destination_connection_id = options_.original_destination_connection_id;
    }
    local_cids_[0].endpoint_index.cid_key = options_.local_connection_id;
    local_cids_[0].sequence_number = 0;
    local_cids_[0].used = true;
    local_cids_[0].advertised = true;
    // Seed the peer-CID pool with the peer's initial Source Connection ID at
    // sequence 0 (RFC 9000 §5.1.1). This CID arrived in the long-header SCID
    // field, not via a NEW_CONNECTION_ID frame, but RFC 9000 §19.16 mandates
    // that the peer not RETIRE_CONNECTION_ID(0) referencing the packet's own
    // DCID; we track it like any other slot so path-CID accounting is uniform.
    remote_cids_[0].cid = options_.remote_connection_id;
    remote_cids_[0].sequence_number = 0;
    remote_cids_[0].in_use = !options_.remote_connection_id.empty();
    remote_cids_[0].used = remote_cids_[0].in_use;
    options_.transport.initial_max_data = options_.recv_flow.conn_recv_limit;
    options_.transport.initial_max_stream_data_bidi_local = options_.recv_flow.stream_buffer_limit;
    options_.transport.initial_max_stream_data_bidi_remote = options_.recv_flow.stream_buffer_limit;
    options_.transport.initial_max_stream_data_uni = options_.recv_flow.stream_buffer_limit;
    options_.transport.initial_max_streams_bidi = options_.max_peer_bidirectional_streams;
    options_.transport.initial_max_streams_uni = options_.max_peer_unidirectional_streams;
    peer_bidi_streams_.concurrent_limit = options_.max_peer_bidirectional_streams;
    peer_bidi_streams_.advertised_limit = options_.max_peer_bidirectional_streams;
    peer_uni_streams_.concurrent_limit = options_.max_peer_unidirectional_streams;
    peer_uni_streams_.advertised_limit = options_.max_peer_unidirectional_streams;
    recv_data_limit_ = options_.recv_flow.conn_recv_limit;
    QuicOutputFramePool &frame_pool =
            options_.output_frame_pool != nullptr ? *options_.output_frame_pool : output_frame_pool_;
    packet_number_spaces_[0].reset(QuicEncryptionLevel::Initial);
    packet_number_spaces_[0].set_frame_pool(frame_pool);
    packet_number_spaces_[0].crypto_recv.init(recv_extent_pool());
    packet_number_spaces_[1].reset(QuicEncryptionLevel::Handshake);
    packet_number_spaces_[1].set_frame_pool(frame_pool);
    packet_number_spaces_[1].crypto_recv.init(recv_extent_pool());
    packet_number_spaces_[2].reset(QuicEncryptionLevel::Application);
    packet_number_spaces_[2].set_frame_pool(frame_pool);
    packet_number_spaces_[2].crypto_recv.init(recv_extent_pool());
    quic_congestion_init(congestion_, QuicTime{0});
    quic_rtt_init(rtt_);

    QuicPath *path = path_manager_.create(options_.remote_addr, options_.local_addr, options_.remote_connection_id,
                                          QuicPathTag::Active);
    if (path != nullptr) {
        path->validated = options_.initial_path_validated;
        (void) path_manager_.set_active(*path);
    }
}

QuicConnection::~QuicConnection() {
    notify_all_local_stream_attach_waiters(common::IoErr::Canceled);
    if (auto *loop = event::EventLoop::current_or_null()) {
        cancel_loss_detection_timer(*loop);
        cancel_key_update_discard_timer(*loop);
        cancel_idle_timer(*loop);
        cancel_close_timer(*loop);
        cancel_keepalive_timer(*loop);
    }
}

common::IoResult<void> QuicConnection::start_handshake() noexcept {
    if (state_ != QuicConnectionState::Init) {
        return std::unexpected(common::IoErr::Already);
    }
    state_ = QuicConnectionState::Handshaking;
    return {};
}

common::IoResult<void> QuicConnection::mark_established() noexcept {
    if (terminal_closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (state_ == QuicConnectionState::GracefulClosing) {
        enter_closing(close_info_);
        return {};
    }
    if (state_ != QuicConnectionState::Init && state_ != QuicConnectionState::Handshaking) {
        return std::unexpected(common::IoErr::Already);
    }
    state_ = QuicConnectionState::Established;
    if (options_.endpoint != nullptr) {
        auto filled = options_.endpoint->fill_local_connection_ids(*this);
        if (!filled) {
            close(QuicErrorCode::InternalError);
            return std::unexpected(filled.error());
        }
    }
    notify_all_local_stream_attach_waiters();
    return {};
}

void QuicConnection::begin_draining(QuicErrorCode error) noexcept {
    enter_draining(QuicCloseInfo{
            .source = QuicCloseSource::PeerConnectionClose,
            .frame_kind = QuicCloseFrameKind::Transport,
            .error_code = static_cast<std::uint64_t>(error),
            .frame_type = 0,
    });
}

void QuicConnection::begin_draining(QuicCloseInfo info) noexcept { enter_draining(info); }

bool QuicConnection::queue_close_frame_for_level(QuicEncryptionLevel level) noexcept {
    if (!can_queue_frame()) {
        return false;
    }
    QuicPacketNumberSpace &space = packet_number_space(level);
    QuicPacketProtectionKeys *keys = quic_packet_keys(crypto_, level, /*write_keys=*/true);
    if (keys == nullptr || !keys->ready) {
        return false;
    }

    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return false;
    }

    // RFC 9000 §10.2.3: Application-level close uses CONNECTION_CLOSE_APP on the
    // Application level. Initial/Handshake levels always use plain
    // CONNECTION_CLOSE, with error code APPLICATION_ERROR.
    if (close_info_.frame_kind == QuicCloseFrameKind::Application && level == QuicEncryptionLevel::Application) {
        frame->type = QuicFrameType::ConnectionCloseApp;
        frame->u.close.error_code = close_info_.error_code;
    } else if (close_info_.frame_kind == QuicCloseFrameKind::Application) {
        // Non-application level: downgrade to transport CC with APPLICATION_ERROR
        frame->type = QuicFrameType::ConnectionClose;
        frame->u.close.error_code = static_cast<std::uint64_t>(QuicErrorCode::ApplicationError);
    } else {
        frame->type = QuicFrameType::ConnectionClose;
        frame->u.close.error_code = close_info_.error_code;
    }
    frame->u.close.frame_type = close_info_.frame_kind == QuicCloseFrameKind::Transport ? close_info_.frame_type : 0;
    frame->u.close.reason = nullptr;
    frame->u.close.reason_length = 0;
    frame->u.close.owned_reason = nullptr;
    frame->ignore_congestion = true;
    frame->ignore_loss = true;

    space.pending_frames.push_back(*frame);
    return true;
}

void QuicConnection::enqueue_close_frames_all_levels() noexcept {
    clear_pending_frames_all_levels();

    bool any_queued = false;
    for (QuicEncryptionLevel level: kCloseLevels) {
        if (queue_close_frame_for_level(level)) {
            any_queued = true;
        }
    }

    // Stamp last_cc_msec_ so requeue_close_frame() rate-limits subsequent
    // retransmissions against this initial send. Matches nginx's qc->last_cc.
    if (any_queued) {
        if (event::EventLoop *loop = event::EventLoop::current_or_null()) {
            last_cc_msec_ = quic_time_ms(loop->now());
        }
    }
}

void QuicConnection::clear_pending_frames_all_levels() noexcept {
    for (QuicEncryptionLevel level: kCloseLevels) {
        QuicPacketNumberSpace &space = packet_number_space(level);
        while (QuicOutputFrame *frame = space.pending_frames.pop_front()) {
            space.release_frame(*frame);
        }
        space.send_ack = false;
        space.send_ack_count = 0;
        space.pending_ack = kUnsetPacketNumber;
    }

    for (QuicPath &path: path_manager_.paths()) {
        if (path.allocated) {
            path_manager_.clear_frames(path);
        }
    }
}

void QuicConnection::close_all_streams(std::uint64_t error_code) noexcept {
    streams_.for_each([error_code](QuicStream &stream) noexcept { stream.close(error_code); });
}

void QuicConnection::clear_packet_space_frames_for_detach(QuicPacketNumberSpace &space) noexcept {
    auto release_queue = [this, &space](QuicOutputFrameQueue &queue, bool drop_stream_tickets) noexcept {
        while (QuicOutputFrame *frame = queue.pop_front()) {
            if (drop_stream_tickets && frame->type == QuicFrameType::Stream) {
                drop_stream_send_ticket(frame->u.stream.stream_id);
            }
            frame->path = nullptr;
            space.release_frame(*frame);
        }
    };

    release_queue(space.pending_frames, true);
    release_queue(space.sending_frames, false);
    release_queue(space.sent_frames, false);

    quic_output_frame_release_data(space.ack_frame);
    space.ack_frame = QuicOutputFrame{};
    space.send_ack = false;
    space.send_ack_count = 0;
    space.pending_ack = kUnsetPacketNumber;
}

void QuicConnection::clear_frames_for_detach() noexcept {
    for (QuicPath &path: path_manager_.paths()) {
        if (path.allocated) {
            path_manager_.clear_frames(path);
        }
    }

    for (QuicPacketNumberSpace &space: packet_number_spaces_) {
        clear_packet_space_frames_for_detach(space);
        space.set_frame_pool(output_frame_pool_);
    }
}

void QuicConnection::close(QuicErrorCode error, std::uint64_t frame_type) noexcept {
    enter_closing(QuicCloseInfo{
            .source = QuicCloseSource::Local,
            .frame_kind = QuicCloseFrameKind::Transport,
            .error_code = static_cast<std::uint64_t>(error),
            .frame_type = frame_type,
    });
}

void QuicConnection::close_immediately(QuicErrorCode error, std::uint64_t frame_type) noexcept {
    enter_closing(
            QuicCloseInfo{
                    .source = QuicCloseSource::Local,
                    .frame_kind = QuicCloseFrameKind::Transport,
                    .error_code = static_cast<std::uint64_t>(error),
                    .frame_type = frame_type,
            },
            true);
}

void QuicConnection::close_application(std::uint64_t error_code) noexcept {
    enter_closing(QuicCloseInfo{
            .source = QuicCloseSource::Local,
            .frame_kind = QuicCloseFrameKind::Application,
            .error_code = error_code,
            .frame_type = 0,
    });
}

void QuicConnection::close_crypto_error(std::uint8_t alert, std::uint64_t frame_type) noexcept {
    enter_closing(
            QuicCloseInfo{
                    .source = QuicCloseSource::Local,
                    .frame_kind = QuicCloseFrameKind::Transport,
                    .error_code = quic_crypto_error_code(alert),
                    .frame_type = frame_type,
            },
            /*immediate=*/true);
}

void QuicConnection::shutdown(QuicErrorCode error, std::uint64_t frame_type, std::chrono::milliseconds grace) noexcept {
    if (state_ == QuicConnectionState::GracefulClosing || terminal_closing()) {
        return;
    }

    QuicCloseInfo info{
            .source = QuicCloseSource::Local,
            .frame_kind = QuicCloseFrameKind::Transport,
            .error_code = static_cast<std::uint64_t>(error),
            .frame_type = frame_type,
    };

    if (active_stream_count() == 0) {
        enter_closing(info);
        return;
    }

    enter_graceful_closing(info, grace);
}

void QuicConnection::shutdown_application(std::uint64_t error_code, std::chrono::milliseconds grace) noexcept {
    if (state_ == QuicConnectionState::GracefulClosing || terminal_closing()) {
        return;
    }

    QuicCloseInfo info{
            .source = QuicCloseSource::Local,
            .frame_kind = QuicCloseFrameKind::Application,
            .error_code = error_code,
            .frame_type = 0,
    };

    if (active_stream_count() == 0) {
        enter_closing(info);
        return;
    }

    enter_graceful_closing(info, grace);
}

void QuicConnection::enter_graceful_closing(QuicCloseInfo info, std::chrono::milliseconds grace) noexcept {
    if (state_ == QuicConnectionState::GracefulClosing || terminal_closing()) {
        return;
    }
    close_info_ = info;
    state_ = QuicConnectionState::GracefulClosing;
    notify_all_local_stream_attach_waiters(common::IoErr::Canceled);

    const std::chrono::milliseconds delay = grace.count() > 0 ? grace : options_.graceful_shutdown_grace;
    if (delay.count() <= 0) {
        return;
    }
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop == nullptr) {
        return;
    }
    if (close_timer_entry_.is_in_heap()) {
        loop->cancel<QuicConnection, &QuicConnection::close_timer_entry_>(*this);
    }
    loop->post_at<QuicConnection, &QuicConnection::close_timer_entry_, &QuicConnection::on_close_timer>(
            loop->now() + delay, *this);
}

void QuicConnection::enter_closing(QuicCloseInfo info, bool immediate) noexcept {
    if (state_ == QuicConnectionState::Closed || state_ == QuicConnectionState::Draining) {
        return;
    }
    if (state_ == QuicConnectionState::Closing) {
        if (immediate) {
            if (event::EventLoop *loop = event::EventLoop::current_or_null()) {
                arm_close_timer_immediate(*loop);
            }
        }
        return;
    }

    if (event::EventLoop *loop = event::EventLoop::current_or_null()) {
        if (close_timer_entry_.is_in_heap()) {
            loop->cancel<QuicConnection, &QuicConnection::close_timer_entry_>(*this);
        }
    }

    close_info_ = info;
    state_ = QuicConnectionState::Closing;
    notify_all_local_stream_attach_waiters(common::IoErr::Canceled);

    enqueue_close_frames_all_levels();
    close_all_streams(close_info_.error_code);

    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop != nullptr) {
        schedule_send();
        if (immediate) {
            arm_close_timer_immediate(*loop);
        } else {
            arm_close_timer(*loop);
        }
    }
}

void QuicConnection::enter_draining(QuicCloseInfo info) noexcept {
    if (state_ == QuicConnectionState::Closed || state_ == QuicConnectionState::Draining) {
        return;
    }
    if (event::EventLoop *loop = event::EventLoop::current_or_null()) {
        if (close_timer_entry_.is_in_heap()) {
            loop->cancel<QuicConnection, &QuicConnection::close_timer_entry_>(*this);
        }
    }

    close_info_ = info;
    state_ = QuicConnectionState::Draining;
    notify_all_local_stream_attach_waiters(common::IoErr::Canceled);
    clear_pending_frames_all_levels();
    close_all_streams(close_info_.error_code);

    if (event::EventLoop *loop = event::EventLoop::current_or_null()) {
        arm_close_timer(*loop);
    }
}

void QuicConnection::enter_closed() noexcept {
    if (state_ == QuicConnectionState::Closed) {
        return;
    }

    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop != nullptr) {
        cancel_loss_detection_timer(*loop);
        cancel_key_update_discard_timer(*loop);
        cancel_idle_timer(*loop);
        cancel_close_timer(*loop);
        cancel_keepalive_timer(*loop);
        path_manager_.cancel_validation_timer(*loop);
    }

    state_ = QuicConnectionState::Closed;
    notify_all_local_stream_attach_waiters(common::IoErr::Canceled);
    close_all_streams(close_info_.error_code);
    streams_.clear();

    if (options_.on_close_timeout != nullptr) {
        options_.on_close_timeout(options_.lifecycle_owner, *this);
    }
}

void QuicConnection::maybe_finish_graceful_close() noexcept {
    if (state_ != QuicConnectionState::GracefulClosing || active_stream_count() > 0) {
        return;
    }
    enter_closing(close_info_);
}

void QuicConnection::requeue_close_frame(QuicEncryptionLevel level) noexcept {
    if (state_ != QuicConnectionState::Closing) {
        return;
    }

    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop == nullptr) {
        return;
    }

    const std::chrono::milliseconds now = quic_time_ms(loop->now());
    if (last_cc_msec_.count() > 0 && now - last_cc_msec_ < kQuicCloseFrameMinInterval) {
        return; // rate-limited
    }

    // Clear any existing CC frame on this level first (N + 1 duplicates)
    QuicPacketNumberSpace &space = packet_number_space(level);
    QuicOutputFrame *existing = space.pending_frames.front();
    while (existing != nullptr) {
        QuicOutputFrame *next = space.pending_frames.next_of(*existing);
        if (existing->type == QuicFrameType::ConnectionClose || existing->type == QuicFrameType::ConnectionCloseApp) {
            space.pending_frames.erase(*existing);
            space.release_frame(*existing);
        }
        existing = next;
    }

    if (queue_close_frame_for_level(level)) {
        last_cc_msec_ = now;
        schedule_send();
    }
}

void QuicConnection::mark_closed() noexcept {
    if (state_ == QuicConnectionState::Closed) {
        return;
    }
    state_ = QuicConnectionState::Closed;
    notify_all_local_stream_attach_waiters(common::IoErr::Canceled);
    close_all_streams(close_info_.error_code);
    streams_.clear();
}

std::chrono::milliseconds QuicConnection::effective_idle_timeout() const noexcept {
    return options_.transport.max_idle_timeout;
}

void QuicConnection::arm_idle_timer(event::EventLoop &loop) noexcept {
    if (idle_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::idle_timer_entry_>(*this);
    }

    const std::chrono::milliseconds timeout = effective_idle_timeout();
    if (timeout.count() <= 0 || state_ == QuicConnectionState::Closed) {
        return;
    }

    loop.post_at<QuicConnection, &QuicConnection::idle_timer_entry_, &QuicConnection::on_idle_timer>(
            loop.now() + timeout, *this);
}

void QuicConnection::cancel_idle_timer(event::EventLoop &loop) noexcept {
    if (idle_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::idle_timer_entry_>(*this);
    }
    idle_send_timer_set_ = false;
}

void QuicConnection::arm_close_timer(event::EventLoop &loop) noexcept {
    if (close_timer_entry_.is_in_heap()) {
        return;
    }
    if (state_ == QuicConnectionState::Closed) {
        return;
    }

    cancel_idle_timer(loop);
    cancel_loss_detection_timer(loop);
    cancel_key_update_discard_timer(loop);
    cancel_keepalive_timer(loop);
    path_manager_.cancel_validation_timer(loop);

    QuicTime pto = quic_pto(rtt_, peer_transport_.params.max_ack_delay, state_ == QuicConnectionState::Established,
                            state_ == QuicConnectionState::Established);
    if (pto <= QuicTime{0}) {
        pto = QuicTime{1};
    }

    loop.post_at<QuicConnection, &QuicConnection::close_timer_entry_, &QuicConnection::on_close_timer>(
            loop.now() + pto * 3, *this);
}

void QuicConnection::arm_close_timer_immediate(event::EventLoop &loop) noexcept {
    // Fast-close path: cancel any existing close timer and schedule one for "now"
    // so on_close_timer fires on the next event-loop iteration, after this turn's
    // pending sends have flushed. Mirrors nginx's rc == NGX_ERROR path.
    if (close_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::close_timer_entry_>(*this);
    }
    if (state_ == QuicConnectionState::Closed) {
        return;
    }

    cancel_idle_timer(loop);
    cancel_loss_detection_timer(loop);
    cancel_key_update_discard_timer(loop);
    cancel_keepalive_timer(loop);
    path_manager_.cancel_validation_timer(loop);

    loop.post_at<QuicConnection, &QuicConnection::close_timer_entry_, &QuicConnection::on_close_timer>(loop.now(),
                                                                                                       *this);
}

void QuicConnection::cancel_close_timer(event::EventLoop &loop) noexcept {
    if (close_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::close_timer_entry_>(*this);
    }
}

std::chrono::milliseconds QuicConnection::keepalive_delay() const noexcept {
    std::chrono::milliseconds delay = options_.keepalive_interval;
    if (delay.count() <= 0) {
        return std::chrono::milliseconds{0};
    }

    const std::chrono::milliseconds idle = effective_idle_timeout();
    if (idle.count() > 0 && delay >= idle) {
        delay = idle / 2;
        if (delay.count() <= 0) {
            delay = idle;
        }
    }
    return delay;
}

void QuicConnection::arm_keepalive_timer(event::EventLoop &loop) noexcept {
    if (keepalive_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::keepalive_timer_entry_>(*this);
    }
    if (state_ != QuicConnectionState::Established || !crypto_.application_write.ready) {
        return;
    }

    const std::chrono::milliseconds delay = keepalive_delay();
    if (delay.count() <= 0) {
        return;
    }

    loop.post_at<QuicConnection, &QuicConnection::keepalive_timer_entry_, &QuicConnection::on_keepalive_timer>(
            loop.now() + delay, *this);
}

void QuicConnection::cancel_keepalive_timer(event::EventLoop &loop) noexcept {
    if (keepalive_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::keepalive_timer_entry_>(*this);
    }
}

void QuicConnection::on_packet_processed(event::EventLoop &loop) noexcept {
    if (closing()) {
        return;
    }
    idle_send_timer_set_ = false;
    arm_idle_timer(loop);
    arm_keepalive_timer(loop);
}

void QuicConnection::on_ack_eliciting_packet_sent(event::EventLoop &loop) noexcept {
    if (closing()) {
        return;
    }
    if (!idle_send_timer_set_) {
        idle_send_timer_set_ = true;
        arm_idle_timer(loop);
    }
    arm_keepalive_timer(loop);
}

void QuicConnection::arm_loss_detection_timer(event::EventLoop &loop) noexcept {
    if (loss_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::loss_timer_entry_>(*this);
    }

    if (closing()) {
        loss_timer_mode_ = QuicLossTimerMode::None;
        return;
    }

    const QuicTime now = quic_time_ms(loop.now());
    const QuicLossDetectionTimer timer = quic_loss_detection_timer(*this, now, pto_count_);
    loss_timer_mode_ = timer.mode;
    if (timer.mode == QuicLossTimerMode::None) {
        return;
    }

    loop.post_at<QuicConnection, &QuicConnection::loss_timer_entry_, &QuicConnection::on_loss_detection_timer>(
            loop.now() + timer.delay, *this);
}

void QuicConnection::cancel_loss_detection_timer(event::EventLoop &loop) noexcept {
    if (loss_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::loss_timer_entry_>(*this);
    }
    loss_timer_mode_ = QuicLossTimerMode::None;
}


void QuicConnection::arm_key_update_discard_timer(event::EventLoop &loop) noexcept {
    if (key_update_discard_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::key_update_discard_timer_entry_>(*this);
    }
    if (closing() || !crypto_.previous_application_keys_ready) {
        return;
    }
    // RFC 9001 §6.5: retain old keys for at least three times the PTO so that
    // reordered packets carrying the previous key phase can still be decrypted.
    const QuicTime pto = quic_pto(rtt_, std::chrono::duration_cast<QuicTime>(peer_transport_.params.max_ack_delay),
                                  true, state_ == QuicConnectionState::Established);
    const QuicTime delay = pto * 3;
    loop.post_at<QuicConnection, &QuicConnection::key_update_discard_timer_entry_,
                 &QuicConnection::on_key_update_discard_timer>(loop.now() + delay, *this);
}

void QuicConnection::cancel_key_update_discard_timer(event::EventLoop &loop) noexcept {
    if (key_update_discard_timer_entry_.is_in_heap()) {
        loop.cancel<QuicConnection, &QuicConnection::key_update_discard_timer_entry_>(*this);
    }
}

void QuicConnection::on_key_update_discard_timer(QuicConnection *connection) noexcept {
    if (connection == nullptr) {
        return;
    }
    connection->crypto_.previous_application_read.reset();
    connection->crypto_.previous_application_keys_ready = false;
}

void QuicConnection::on_idle_timer(QuicConnection *connection) noexcept {
    if (connection == nullptr || connection->state_ == QuicConnectionState::Closed) {
        return;
    }

    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop != nullptr) {
        connection->cancel_loss_detection_timer(*loop);
        connection->cancel_key_update_discard_timer(*loop);
        connection->cancel_close_timer(*loop);
        connection->cancel_keepalive_timer(*loop);
        connection->path_manager_.cancel_validation_timer(*loop);
    }

    // RFC 9000 §10.1: idle timeout is silent — discard state without sending CC.
    connection->idle_send_timer_set_ = false;
    connection->close_info_ = QuicCloseInfo{
            .source = QuicCloseSource::IdleTimeout,
            .frame_kind = QuicCloseFrameKind::Transport,
            .error_code = static_cast<std::uint64_t>(QuicErrorCode::NoError),
            .frame_type = 0,
    };
    connection->state_ = QuicConnectionState::Closed;
    connection->notify_all_local_stream_attach_waiters(common::IoErr::Canceled);
    connection->close_all_streams(connection->close_info_.error_code);
    connection->streams_.clear();

    if (connection->options_.on_idle_timeout != nullptr) {
        connection->options_.on_idle_timeout(connection->options_.lifecycle_owner, *connection);
    }
}

void QuicConnection::on_close_timer(QuicConnection *connection) noexcept {
    if (connection == nullptr || connection->state_ == QuicConnectionState::Closed) {
        return;
    }

    switch (connection->state_) {
        case QuicConnectionState::GracefulClosing:
            connection->enter_closing(connection->close_info_);
            return;
        case QuicConnectionState::Closing:
        case QuicConnectionState::Draining:
            connection->enter_closed();
            return;
        default:
            return;
    }
}

void QuicConnection::on_keepalive_timer(QuicConnection *connection) noexcept {
    if (connection == nullptr || connection->state_ != QuicConnectionState::Established ||
        !connection->crypto_.application_write.ready) {
        return;
    }

    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop == nullptr) {
        return;
    }

    if (connection->has_pending_send_work()) {
        connection->arm_keepalive_timer(*loop);
        return;
    }

    auto queued = connection->queue_ping_frame();
    if (!queued) {
        connection->close(QuicErrorCode::InternalError);
        connection->arm_close_timer(*loop);
    }
}


void QuicConnection::on_loss_detection_timer(QuicConnection *connection) noexcept {
    if (connection == nullptr) {
        return;
    }

    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop == nullptr) {
        connection->loss_timer_mode_ = QuicLossTimerMode::None;
        return;
    }

    if (connection->closing()) {
        connection->loss_timer_mode_ = QuicLossTimerMode::None;
        return;
    }

    const QuicTime now = quic_time_ms(loop->now());
    const QuicLossTimerMode mode = connection->loss_timer_mode_;

    if (mode == QuicLossTimerMode::Lost) {
        auto lost = quic_detect_lost(*connection, now, nullptr);
        if (!lost) {
            connection->close(QuicErrorCode::InternalError);
            connection->loss_timer_mode_ = QuicLossTimerMode::None;
            return;
        }

        if (lost->unblocked || lost->lost_frames || lost->force_send) {
            connection->schedule_send();
        }

    } else if (mode == QuicLossTimerMode::Pto) {
        auto queued = quic_queue_pto_probe_frames(*connection, now, connection->pto_count_);
        if (!queued) {
            connection->close(QuicErrorCode::InternalError);
            connection->loss_timer_mode_ = QuicLossTimerMode::None;
            return;
        }

        if (*queued) {
            ++connection->pto_count_;
            connection->schedule_send();
        }
    }

    connection->arm_loss_detection_timer(*loop);
}


common::IoResult<QuicStream *> QuicConnection::attach_stream(QuicStream::Lease &&lease, std::uint64_t stream_id,
                                                             QuicStreamRecvQueue::Options recv_options) noexcept {
    if (!lease) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (lease->attached_to_connection() || lease->stream_id_assigned()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (streams_.find(stream_id) != nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!streams_.reserve_for_insert()) {
        return std::unexpected(common::IoErr::NoMem);
    }

    QuicStream *stream = lease.get();
    stream->assign_conn_ctx(*this, stream_id, recv_options);

    if (!streams_.insert(std::move(lease))) {
        stream->detach_from_connection();
        return std::unexpected(common::IoErr::Invalid);
    }
    return stream;
}

common::IoResult<QuicStream *> QuicConnection::try_attach_local_stream(QuicStream::Lease &&stream,
                                                                       QuicStreamType type) noexcept {
    if (!stream) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (!accepting_new_streams()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (state_ != QuicConnectionState::Established) {
        return std::unexpected(common::IoErr::Busy);
    }
    if (event::EventLoop *current = event::EventLoop::current_or_null()) {
        FIBER_ASSERT(loop_ == nullptr || current == loop_);
    }
    if (stream->attached_to_connection() || stream->stream_id_assigned()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint64_t &next =
            type == QuicStreamType::Bidirectional ? next_local_bidi_stream_id_ : next_local_uni_stream_id_;
    const std::uint64_t limit = local_stream_limit(type);
    if (stream_sequence(next) >= limit) {
        auto queued = queue_streams_blocked_frame(type, limit);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        return std::unexpected(common::IoErr::Busy);
    }

    QuicStreamRecvQueue::Options recv_options{
            .buffer_limit = options_.recv_flow.stream_buffer_limit,
            .low_water = options_.recv_flow.stream_low_water,
            .max_stream_data = options_.recv_flow.stream_buffer_limit,
    };
    const std::uint64_t id = next;
    auto attached = attach_stream(std::move(stream), id, recv_options);
    if (!attached) {
        return std::unexpected(attached.error());
    }

    next += kStreamIncrement;
    return *attached;
}

async::Task<common::IoResult<QuicStream *>>
QuicConnection::attach_local_stream(QuicStream::Lease stream, QuicStreamType type,
                                    std::chrono::milliseconds timeout) noexcept {
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }

    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool deadline_set = timeout == std::chrono::milliseconds::max();

    for (;;) {
        auto attached = try_attach_local_stream(std::move(stream), type);
        if (attached || attached.error() != common::IoErr::Busy) {
            co_return attached;
        }
        if (timeout == std::chrono::milliseconds::zero()) {
            co_return std::unexpected(common::IoErr::TimedOut);
        }
        if (!deadline_set) {
            auto *loop = event::EventLoop::current_or_null();
            FIBER_ASSERT(loop != nullptr);
            FIBER_ASSERT(loop_ == nullptr || loop == loop_);
            deadline = loop->now() + timeout;
            deadline_set = true;
        }

        common::IoErr wait_result = co_await LocalStreamAttachAwaiter(*this, type, deadline);
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
    }
}

common::IoResult<std::uint64_t> QuicConnection::next_local_stream_id(QuicStreamType type) noexcept {
    if (!accepting_new_streams()) {
        return std::unexpected(common::IoErr::Canceled);
    }

    std::uint64_t &next =
            type == QuicStreamType::Bidirectional ? next_local_bidi_stream_id_ : next_local_uni_stream_id_;
    const std::uint64_t limit = local_stream_limit(type);
    if (stream_sequence(next) >= limit) {
        auto queued = queue_streams_blocked_frame(type, limit);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        return std::unexpected(common::IoErr::Busy);
    }

    const std::uint64_t id = next;
    next += kStreamIncrement;
    return id;
}

bool QuicConnection::can_accept_peer_stream(std::uint64_t stream_id) const noexcept {
    if (!accepting_new_streams()) {
        return false;
    }
    if (!is_peer_stream(stream_id)) {
        return false;
    }
    const QuicStreamType type = stream_type(stream_id);
    const PeerStreamLimitWindow &window = peer_stream_window(type);
    const std::uint64_t sequence = stream_sequence(stream_id);
    if (sequence >= window.advertised_limit) {
        return false;
    }
    if (sequence >= window.opened_count && window.opened_count - window.retired_count >= window.concurrent_limit) {
        return false;
    }
    return true;
}

common::IoResult<void> QuicConnection::record_peer_stream_id(std::uint64_t stream_id) noexcept {
    if (!can_accept_peer_stream(stream_id)) {
        return std::unexpected(common::IoErr::Busy);
    }

    // Advance the per-type high-water mark to include this stream id. RFC 9000
    // §2.1: a stream ID used out of order opens all lower-numbered streams of
    // that type — they are REAL active streams (created by
    // get_or_create_peer_stream), not retired. Only on_peer_stream_retired
    // increments retired_count; the gap must NOT inflate it (the previous code
    // did, which caused the implicit streams to be skipped and their later data
    // silently dropped). nginx advances client_streams_bidi to (id>>2)+1
    // unconditionally before its create loop (ngx_quic_get_stream).
    const QuicStreamType type = stream_type(stream_id);
    PeerStreamLimitWindow &window = peer_stream_window(type);
    const std::uint64_t opened = stream_sequence(stream_id) + 1;
    if (opened > window.opened_count) {
        window.opened_count = opened;
    }
    return {};
}

QuicStream *QuicConnection::find_stream(std::uint64_t stream_id) noexcept { return streams_.find(stream_id); }

const QuicStream *QuicConnection::find_stream(std::uint64_t stream_id) const noexcept {
    return streams_.find(stream_id);
}

common::IoResult<QuicStream *> QuicConnection::create_peer_stream(std::uint64_t stream_id) noexcept {
    QuicStreamRecvQueue::Options recv_options{
            .buffer_limit = options_.recv_flow.stream_buffer_limit,
            .low_water = options_.recv_flow.stream_low_water,
            .max_stream_data = options_.recv_flow.stream_buffer_limit,
    };
    QuicStream::Lease lease = options_.ops.create_stream(options_.owner);
    auto attached = attach_stream(std::move(lease), stream_id, recv_options);
    if (!attached) {
        return std::unexpected(attached.error());
    }

    if (options_.ops.on_peer_stream_attached != nullptr) {
        options_.ops.on_peer_stream_attached(options_.owner, **attached);
    }
    return *attached;
}

common::IoResult<QuicStream *> QuicConnection::get_or_create_peer_stream(std::uint64_t stream_id) noexcept {
    if (!accepting_new_streams()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (QuicStream *stream = streams_.find(stream_id)) {
        return stream;
    }

    const QuicStreamType type = stream_type(stream_id);
    PeerStreamLimitWindow &window = peer_stream_window(type);
    // Capture the gap lower bound BEFORE record_peer_stream_id advances the
    // high-water mark; every sequence in [from_seq, target_seq] that is not
    // already in the table must be created (RFC 9000 §2.1).
    const std::uint64_t from_seq = window.opened_count;

    auto recorded = record_peer_stream_id(stream_id);
    if (!recorded) {
        return std::unexpected(recorded.error());
    }

    if (options_.ops.create_stream == nullptr) {
        return std::unexpected(common::IoErr::NotSupported);
    }

    // RFC 9000 §2.1 / §4.6: a stream ID used out of order opens all
    // lower-numbered streams of that type. nginx (ngx_quic_get_stream) advances
    // the per-type count to (id>>2)+1 then creates every intermediate stream in
    // a loop, notifying the application for each. They are real open streams
    // (open state, no data, no FIN); frames arriving later for them are
    // delivered, not dropped. Intermediate ids are always below the target,
    // hence below the advertised limit (callers close with STREAM_LIMIT_ERROR
    // on seq >= advertised_limit), so they are always creatable.
    const std::uint64_t target_seq = stream_sequence(stream_id);
    const std::uint64_t type_bits = stream_id & 3ULL;
    const std::uint64_t lower = from_seq < target_seq ? from_seq : target_seq;
    QuicStream *target = nullptr;
    for (std::uint64_t seq = lower; seq <= target_seq; ++seq) {
        const std::uint64_t id = (seq << 2) | type_bits;
        if (QuicStream *existing = streams_.find(id)) {
            if (id == stream_id) {
                target = existing;
            }
            continue;
        }
        auto created = create_peer_stream(id);
        if (!created) {
            return std::unexpected(created.error());
        }
        if (id == stream_id) {
            target = *created;
        }
    }

    if (target == nullptr) {
        return std::unexpected(common::IoErr::Unknown);
    }
    return target;
}

common::IoResult<void> QuicConnection::recv_stream_frame(const QuicStreamFrame &frame, QuicSlice data) noexcept {
    // RFC 9000 §4.1: STREAM data is sent by the stream sender. On a
    // unidirectional stream the local side initiated, the local side is the
    // sender and the peer is the receiver, so the peer MUST NOT send STREAM
    // data on it. nginx closes with STREAM_STATE_ERROR at the top of
    // ngx_quic_handle_stream_frame.
    if (is_local_stream(frame.stream_id) && is_unidirectional_stream(frame.stream_id)) {
        close(QuicErrorCode::StreamStateError, static_cast<std::uint64_t>(QuicFrameType::Stream));
        return std::unexpected(common::IoErr::Busy);
    }

    if (find_stream(frame.stream_id) == nullptr && is_gone_peer_stream(frame.stream_id)) {
        return {};
    }

    // RFC 9000 §4.6: a stream ID at or beyond the advertised max_streams is a
    // STREAM_LIMIT_ERROR. The concurrent-active-stream window is a separate,
    // non-fatal gate handled by get_or_create_peer_stream (returns Busy) and is
    // NOT closed here.
    if (find_stream(frame.stream_id) == nullptr && peer_stream_exceeds_advertised_limit(frame.stream_id)) {
        close(QuicErrorCode::StreamLimitError, static_cast<std::uint64_t>(QuicFrameType::Stream));
        return std::unexpected(common::IoErr::Busy);
    }

    auto stream = get_or_create_peer_stream(frame.stream_id);
    if (!stream) {
        return std::unexpected(stream.error());
    }

    if (frame.offset > std::numeric_limits<std::uint64_t>::max() - data.len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint64_t old_end = (*stream)->recv_queue_.received_end_offset();
    const std::uint64_t frame_end = frame.offset + data.len;
    const std::uint64_t pending_delta = frame_end > old_end ? frame_end - old_end : 0;

    // Connection-level flow control (max_data). nginx's ngx_quic_control_flow
    // closes with FLOW_CONTROL_ERROR when qc->streams.recv_last exceeds
    // recv_max_data.
    auto allowed = check_recv_data_delta(pending_delta);
    if (!allowed) {
        close(QuicErrorCode::FlowControlError, static_cast<std::uint64_t>(QuicFrameType::Stream));
        return std::unexpected(allowed.error());
    }

    // Per-stream flow control (max_stream_data). nginx gates this on
    // recv_state == RECV (final size not yet known); once the final size is
    // known, data beyond it is a FINAL_SIZE_ERROR, handled below.
    if (!(*stream)->has_final_size() && frame_end > (*stream)->recv_queue_.max_stream_data()) {
        close(QuicErrorCode::FlowControlError, static_cast<std::uint64_t>(QuicFrameType::Stream));
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    // FINAL_SIZE_ERROR: data extends beyond an already-known final size
    // (set by a prior FIN or RESET_STREAM).
    if ((*stream)->has_final_size() && frame_end > (*stream)->final_size()) {
        close(QuicErrorCode::FinalSizeError, static_cast<std::uint64_t>(QuicFrameType::Stream));
        return std::unexpected(common::IoErr::Invalid);
    }

    if (frame.fin) {
        // FIN final size conflicts with a previously-known final size.
        if ((*stream)->has_final_size() && (*stream)->final_size() != frame_end) {
            close(QuicErrorCode::FinalSizeError, static_cast<std::uint64_t>(QuicFrameType::Stream));
            return std::unexpected(common::IoErr::Invalid);
        }
        // FIN final size is smaller than data already received on the stream
        // (recv_last > last in nginx).
        if ((*stream)->recv_queue_.received_end_offset() > frame_end) {
            close(QuicErrorCode::FinalSizeError, static_cast<std::uint64_t>(QuicFrameType::Stream));
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    auto received = (*stream)->on_stream_data_recv(data.data, data.len, frame.offset, frame.fin);
    if (!received) {
        return std::unexpected(received.error());
    }
    commit_recv_data_delta(*received);
    maybe_extend_recv_data_flow_control();
    try_release_stream(**stream);
    return {};
}

common::IoResult<void> QuicConnection::recv_reset_stream_frame(const QuicResetStreamFrame &frame) noexcept {
    // RFC 9000 §4.1: RESET_STREAM is sent by the stream sender. On a
    // unidirectional stream the local side initiated, the peer is the receiver
    // and MUST NOT reset it → STREAM_STATE_ERROR (nginx
    // ngx_quic_handle_reset_stream_frame).
    if (is_local_stream(frame.id) && is_unidirectional_stream(frame.id)) {
        close(QuicErrorCode::StreamStateError, static_cast<std::uint64_t>(QuicFrameType::ResetStream));
        return std::unexpected(common::IoErr::Busy);
    }

    if (find_stream(frame.id) == nullptr && is_gone_peer_stream(frame.id)) {
        return {};
    }

    if (find_stream(frame.id) == nullptr && peer_stream_exceeds_advertised_limit(frame.id)) {
        close(QuicErrorCode::StreamLimitError, static_cast<std::uint64_t>(QuicFrameType::ResetStream));
        return std::unexpected(common::IoErr::Busy);
    }

    auto stream = get_or_create_peer_stream(frame.id);
    if (!stream) {
        return std::unexpected(stream.error());
    }

    const std::uint64_t old_end = (*stream)->recv_queue_.received_end_offset();
    // FINAL_SIZE_ERROR: reset final size is smaller than data already received
    // (recv_last > final_size in nginx). Checked before the connection-flow
    // delta to avoid underflow in the subtraction below.
    if (frame.final_size < old_end) {
        close(QuicErrorCode::FinalSizeError, static_cast<std::uint64_t>(QuicFrameType::ResetStream));
        return std::unexpected(common::IoErr::Invalid);
    }
    // Connection-level flow control (max_data). nginx skips the per-stream
    // max_stream_data check for RESET_STREAM (recv_state is RESET_RECVD before
    // ngx_quic_control_flow runs), so only the connection-level limit applies.
    auto allowed = check_recv_data_delta(frame.final_size - old_end);
    if (!allowed) {
        close(QuicErrorCode::FlowControlError, static_cast<std::uint64_t>(QuicFrameType::ResetStream));
        return std::unexpected(allowed.error());
    }
    // FINAL_SIZE_ERROR: reset final size conflicts with a previously-known
    // final size (recv_final_size != final_size in nginx).
    if ((*stream)->has_final_size() && (*stream)->final_size() != frame.final_size) {
        close(QuicErrorCode::FinalSizeError, static_cast<std::uint64_t>(QuicFrameType::ResetStream));
        return std::unexpected(common::IoErr::Invalid);
    }

    auto reset = (*stream)->on_remote_reset(frame.error_code, frame.final_size);
    if (!reset) {
        return std::unexpected(reset.error());
    }
    commit_recv_data_delta(*reset);
    maybe_extend_recv_data_flow_control();
    try_release_stream(**stream);
    return {};
}

common::IoResult<void> QuicConnection::recv_stop_sending_frame(const QuicStopSendingFrame &frame) noexcept {
    // RFC 9000 §4.1: STOP_SENDING is sent by the stream receiver. On a
    // peer-initiated unidirectional stream the peer is the sender (the local
    // side is the receiver), so the peer MUST NOT send STOP_SENDING on it →
    // STREAM_STATE_ERROR (nginx ngx_quic_handle_stop_sending_frame).
    if (is_peer_stream(frame.id) && is_unidirectional_stream(frame.id)) {
        close(QuicErrorCode::StreamStateError, static_cast<std::uint64_t>(QuicFrameType::StopSending));
        return std::unexpected(common::IoErr::Busy);
    }
    QuicStream *stream = find_stream(frame.id);
    if (stream == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return stream->on_remote_stop_sending(frame.error_code);
}

common::IoResult<void> QuicConnection::recv_max_stream_data_frame(const QuicMaxStreamDataFrame &frame) noexcept {
    // RFC 9000 §4.1: MAX_STREAM_DATA is sent by the stream receiver. On a
    // peer-initiated unidirectional stream the peer is the sender, so it MUST
    // NOT raise the local receive window → STREAM_STATE_ERROR (nginx
    // ngx_quic_handle_max_stream_data_frame).
    if (is_peer_stream(frame.id) && is_unidirectional_stream(frame.id)) {
        close(QuicErrorCode::StreamStateError, static_cast<std::uint64_t>(QuicFrameType::MaxStreamData));
        return std::unexpected(common::IoErr::Busy);
    }
    QuicStream *stream = find_stream(frame.id);
    if (stream == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    stream->on_max_stream_data(frame.limit);
    return {};
}

common::IoResult<void> QuicConnection::recv_max_streams_frame(const QuicMaxStreamsFrame &frame) noexcept {
    if (frame.limit > kQuicMaxStreamLimit) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const QuicStreamType type = frame.bidirectional ? QuicStreamType::Bidirectional : QuicStreamType::Unidirectional;
    std::uint64_t &limit =
            frame.bidirectional ? options_.max_local_bidirectional_streams : options_.max_local_unidirectional_streams;
    if (frame.limit <= limit) {
        return {};
    }

    limit = frame.limit;
    LocalStreamBlockedState &blocked = local_stream_blocked_state(type);
    blocked.reported = false;
    blocked.last_limit = 0;
    notify_local_stream_attach_waiters(type);
    return {};
}

common::IoResult<void> QuicConnection::recv_max_data_frame(const QuicMaxDataFrame &frame) noexcept {
    if (frame.max_data > peer_max_data_) {
        peer_max_data_ = frame.max_data;
        notify_peer_data_waiters();
    }
    return {};
}

common::IoResult<void> QuicConnection::recv_streams_blocked_frame(const QuicStreamsBlockedFrame &frame) noexcept {
    if (frame.limit > kQuicMaxStreamLimit) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

void QuicConnection::release_stream_app(QuicStream &stream) noexcept {
    stream.mark_app_released();
    try_release_stream(stream);
}

bool QuicConnection::is_local_stream(std::uint64_t stream_id) const noexcept {
    return (stream_id & kStreamInitiatorMask) == local_initiator_bit();
}

bool QuicConnection::is_bidirectional_stream(std::uint64_t stream_id) noexcept {
    return (stream_id & kStreamTypeMask) == 0;
}

bool QuicConnection::is_unidirectional_stream(std::uint64_t stream_id) noexcept {
    return !is_bidirectional_stream(stream_id);
}

QuicStreamType QuicConnection::stream_type(std::uint64_t stream_id) noexcept {
    return is_bidirectional_stream(stream_id) ? QuicStreamType::Bidirectional : QuicStreamType::Unidirectional;
}

QuicPacketNumberSpace &QuicConnection::packet_number_space(QuicEncryptionLevel level) noexcept {
    return packet_number_spaces_[packet_number_space_index(level)];
}

const QuicPacketNumberSpace &QuicConnection::packet_number_space(QuicEncryptionLevel level) const noexcept {
    return packet_number_spaces_[packet_number_space_index(level)];
}

common::IoResult<void> QuicConnection::init_initial_crypto(const QuicConnectionId &original_dcid) noexcept {
    return quic_init_initial_crypto(crypto_, options_.role, original_dcid);
}

common::IoResult<void> QuicConnection::apply_peer_transport_params(const QuicTransportParams &params) noexcept {
    if (peer_transport_.received) {
        return std::unexpected(common::IoErr::Already);
    }
    if (!params.has_initial_source_connection_id) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (params.initial_source_connection_id.size() != options_.remote_connection_id.size() ||
        (params.initial_source_connection_id.size() != 0 &&
         std::memcmp(params.initial_source_connection_id.data(), options_.remote_connection_id.data(),
                     params.initial_source_connection_id.size()) != 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (options_.role == QuicConnectionRole::Server) {
        if (params.has_original_destination_connection_id || params.has_retry_source_connection_id ||
            params.has_stateless_reset_token) {
            return std::unexpected(common::IoErr::Invalid);
        }
    } else {
        if (!params.has_original_destination_connection_id ||
            !connection_id_equal(params.original_destination_connection_id,
                                 options_.original_destination_connection_id)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (options_.has_retry_source_connection_id) {
            if (!params.has_retry_source_connection_id ||
                !connection_id_equal(params.retry_source_connection_id, options_.retry_source_connection_id)) {
                return std::unexpected(common::IoErr::Invalid);
            }
        } else if (params.has_retry_source_connection_id) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (params.max_udp_payload_size < kMinInitialDatagramSize || params.max_udp_payload_size > kQuicMaxUdpPayloadSize ||
        params.active_connection_id_limit < 2 || params.ack_delay_exponent > 20 || params.max_ack_delay >= 16384) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (params.initial_max_streams_bidi > kQuicMaxStreamLimit || params.initial_max_streams_uni > kQuicMaxStreamLimit) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicTransportSettings applied{};
    applied.max_idle_timeout = std::chrono::milliseconds(params.max_idle_timeout);
    applied.max_udp_payload_size = static_cast<std::size_t>(params.max_udp_payload_size);
    applied.initial_max_data = params.initial_max_data;
    applied.initial_max_stream_data_bidi_local = params.initial_max_stream_data_bidi_local;
    applied.initial_max_stream_data_bidi_remote = params.initial_max_stream_data_bidi_remote;
    applied.initial_max_stream_data_uni = params.initial_max_stream_data_uni;
    applied.initial_max_streams_bidi = params.initial_max_streams_bidi;
    applied.initial_max_streams_uni = params.initial_max_streams_uni;
    applied.ack_delay_exponent = params.ack_delay_exponent;
    applied.max_ack_delay = std::chrono::milliseconds(params.max_ack_delay);
    applied.active_connection_id_limit = params.active_connection_id_limit;
    applied.disable_active_migration = params.disable_active_migration;

    peer_transport_.params = applied;
    peer_transport_.received = true;
    peer_max_data_ = params.initial_max_data;
    options_.max_local_bidirectional_streams = params.initial_max_streams_bidi;
    options_.max_local_unidirectional_streams = params.initial_max_streams_uni;
    local_bidi_streams_blocked_ = {};
    local_uni_streams_blocked_ = {};
    streams_.for_each([this](QuicStream &stream) noexcept {
        stream.on_max_stream_data(initial_stream_send_limit(stream.stream_id()));
    });
    notify_peer_data_waiters();
    notify_all_local_stream_attach_waiters();
    const std::chrono::milliseconds peer_idle_timeout(params.max_idle_timeout);
    if (peer_idle_timeout.count() > 0 &&
        (options_.transport.max_idle_timeout.count() <= 0 || peer_idle_timeout < options_.transport.max_idle_timeout)) {
        options_.transport.max_idle_timeout = peer_idle_timeout;
    }
    if (QuicPath *path = active_path(); path != nullptr && path->validated) {
        const QuicTime now = event::EventLoop::current_or_null() != nullptr
                                     ? quic_time_ms(event::EventLoop::current().now())
                                     : QuicTime{0};
        auto discovered = path_manager_.discover_path_mtu(*path, now);
        if (!discovered) {
            return std::unexpected(discovered.error());
        }
    }
    return {};
}

common::IoResult<bool> QuicConnection::recv_retire_connection_id_frame(const QuicRetireConnectionIdFrame &frame,
                                                                       const QuicConnectionId &packet_dcid) noexcept {
    if (options_.endpoint == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return options_.endpoint->retire_local_connection_id_and_resend(*this, frame.sequence_number, packet_dcid);
}

bool QuicConnection::should_retransmit_new_connection_id(std::uint64_t sequence_number) const noexcept {
    const QuicLocalConnectionIdSlot *slot = find_local_connection_id_slot(sequence_number);
    return slot != nullptr && slot->used && slot->advertised;
}

bool QuicConnection::has_active_local_connection_id(const QuicConnectionId &cid) const noexcept {
    for (const QuicLocalConnectionIdSlot &slot: local_cids_) {
        if (!slot.used) {
            continue;
        }
        if (connection_id_equal(slot.endpoint_index.cid_key, cid)) {
            return true;
        }
    }
    return false;
}

common::IoResult<void>
QuicConnection::stateless_reset_token_for(const QuicConnectionId &cid,
                                          std::uint8_t out[kStatelessResetTokenLength]) const noexcept {
    if (options_.endpoint == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return options_.endpoint->create_stateless_reset_token(cid, out);
}

bool QuicConnection::detects_stateless_reset(const std::uint8_t *packet_data, std::size_t packet_len) const noexcept {
    // RFC 9000 §10.3 / §10.3.1: a stateless reset is a short-header datagram
    // whose final 16 bytes equal a stateless_reset_token the peer advertised via
    // NEW_CONNECTION_ID. It must be strictly longer than the token itself (there
    // is at least a header byte preceding the trailing token); this also keeps
    // the tail pointer below in bounds.
    if (packet_data == nullptr || packet_len <= kStatelessResetTokenLength) {
        return false;
    }
    const std::uint8_t *tail = packet_data + packet_len - kStatelessResetTokenLength;
    for (const QuicRemoteConnectionIdSlot &slot: remote_cids_) {
        if (!slot.in_use || slot.sequence_number == 0) {
            // No stateless reset token is carried for the initial Connection ID;
            // tokens arrive only in NEW_CONNECTION_ID frames (RFC 9000 §10.3.1),
            // so the sequence-0 slot's token is always zero and must be skipped.
            continue;
        }
        // Constant-time compare (cf. nginx ngx_quic_handle_stateless_reset):
        // accumulate the XOR of every byte and match iff the aggregate is zero.
        // Do NOT short-circuit per-byte or use std::memcmp on the secret token.
        std::uint8_t diff = 0;
        for (std::size_t i = 0; i < kStatelessResetTokenLength; ++i) {
            diff |= static_cast<std::uint8_t>(tail[i] ^ slot.stateless_reset_token[i]);
        }
        if (diff == 0) {
            return true;
        }
    }
    return false;
}

std::size_t QuicConnection::active_local_connection_id_count() const noexcept {
    std::size_t count = 0;
    for (const QuicLocalConnectionIdSlot &slot: local_cids_) {
        if (slot.used) {
            ++count;
        }
    }
    return count;
}

std::size_t QuicConnection::local_connection_id_target() const noexcept {
    if (!peer_transport_.received) {
        return 1;
    }
    const std::uint64_t peer_limit = peer_transport_.params.active_connection_id_limit;
    const std::uint64_t capped =
            std::min<std::uint64_t>(peer_limit, static_cast<std::uint64_t>(kQuicLocalConnectionIdSlotCount));
    return static_cast<std::size_t>(capped);
}

QuicLocalConnectionIdSlot *QuicConnection::find_local_connection_id_slot(std::uint64_t sequence_number) noexcept {
    for (QuicLocalConnectionIdSlot &slot: local_cids_) {
        if (slot.used && slot.sequence_number == sequence_number) {
            return &slot;
        }
    }
    return nullptr;
}

const QuicLocalConnectionIdSlot *
QuicConnection::find_local_connection_id_slot(std::uint64_t sequence_number) const noexcept {
    for (const QuicLocalConnectionIdSlot &slot: local_cids_) {
        if (slot.used && slot.sequence_number == sequence_number) {
            return &slot;
        }
    }
    return nullptr;
}

QuicLocalConnectionIdSlot *QuicConnection::find_free_local_connection_id_slot() noexcept {
    for (QuicLocalConnectionIdSlot &slot: local_cids_) {
        if (!slot.used) {
            return &slot;
        }
    }
    return nullptr;
}

common::IoResult<void>
QuicConnection::queue_new_connection_id_frame(const QuicLocalConnectionIdSlot &slot,
                                              const std::uint8_t token[kStatelessResetTokenLength]) noexcept {
    if (!can_queue_frame()) {
        return {};
    }
    if (!slot.used || token == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const QuicConnectionId &cid = slot.endpoint_index.cid_key;
    if (cid.empty() || cid.size() > kMaxConnectionIdLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicPacketNumberSpace &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::NewConnectionId;
    frame->u.new_connection_id.sequence_number = slot.sequence_number;
    frame->u.new_connection_id.retire_prior_to = 0;
    frame->u.new_connection_id.cid_len = static_cast<std::uint8_t>(cid.size());
    std::memcpy(frame->u.new_connection_id.cid, cid.data(), cid.size());
    std::memcpy(frame->u.new_connection_id.stateless_reset_token, token, kStatelessResetTokenLength);
    space.pending_frames.push_back(*frame);
    return {};
}

common::IoResult<void> QuicConnection::queue_retire_connection_id_frame(std::uint64_t sequence_number) noexcept {
    if (!can_queue_frame()) {
        return {};
    }
    QuicPacketNumberSpace &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::RetireConnectionId;
    frame->u.retire_connection_id.sequence_number = sequence_number;
    space.pending_frames.push_back(*frame);
    return {};
}

QuicRemoteConnectionIdSlot *QuicConnection::find_remote_connection_id_slot(std::uint64_t sequence_number) noexcept {
    for (QuicRemoteConnectionIdSlot &slot: remote_cids_) {
        if (slot.in_use && slot.sequence_number == sequence_number) {
            return &slot;
        }
    }
    return nullptr;
}

const QuicRemoteConnectionIdSlot *
QuicConnection::find_remote_connection_id_slot(std::uint64_t sequence_number) const noexcept {
    for (const QuicRemoteConnectionIdSlot &slot: remote_cids_) {
        if (slot.in_use && slot.sequence_number == sequence_number) {
            return &slot;
        }
    }
    return nullptr;
}

QuicRemoteConnectionIdSlot *QuicConnection::find_free_remote_connection_id_slot() noexcept {
    for (QuicRemoteConnectionIdSlot &slot: remote_cids_) {
        if (!slot.in_use) {
            return &slot;
        }
    }
    return nullptr;
}

std::size_t QuicConnection::active_remote_connection_id_count() const noexcept {
    std::size_t count = 0;
    for (const QuicRemoteConnectionIdSlot &slot: remote_cids_) {
        if (slot.in_use) {
            ++count;
        }
    }
    return count;
}

bool QuicConnection::should_retransmit_retire_connection_id(std::uint64_t sequence_number) const noexcept {
    // Only retransmit a lost RETIRE_CONNECTION_ID when the CID it refers to is
    // no longer in our pool. If the slot is still in_use, the peer will (or
    // already did) re-issue it via NEW_CONNECTION_ID and we'll re-RETIRE.
    return find_remote_connection_id_slot(sequence_number) == nullptr;
}

common::IoResult<bool> QuicConnection::retire_remote_connection_id(QuicRemoteConnectionIdSlot &slot) noexcept {
    if (!slot.in_use) {
        return false;
    }

    bool queued = false;
    if (slot.used) {
        QuicPath *path = path_manager_.find_path_by_remote_cid_sequence(slot.sequence_number);
        if (path != nullptr) {
            if (path == path_manager_.active()) {
                // Active path needs a replacement CID, otherwise we cannot keep
                // sending; if none available the connection has run out of
                // peer-provided CIDs and must close (RFC 9000 §5.1.2 — implicit:
                // a peer that retires more than it issues breaks the protocol).
                QuicRemoteConnectionIdSlot *replacement = nullptr;
                for (QuicRemoteConnectionIdSlot &candidate: remote_cids_) {
                    if (&candidate == &slot) {
                        continue;
                    }
                    if (candidate.in_use && !candidate.used) {
                        replacement = &candidate;
                        break;
                    }
                }
                if (replacement == nullptr) {
                    close(QuicErrorCode::InternalError);
                    return std::unexpected(common::IoErr::Invalid);
                }
                replacement->used = true;
                (void) path_manager_.rebind_paths_to_cid(slot.sequence_number, *replacement);
            } else {
                path_manager_.free(*path);
            }
        }
    }

    const std::uint64_t seq = slot.sequence_number;
    slot = QuicRemoteConnectionIdSlot{};
    auto enqueued = queue_retire_connection_id_frame(seq);
    if (!enqueued) {
        return std::unexpected(enqueued.error());
    }
    queued = true;
    return queued;
}

common::IoResult<bool> QuicConnection::recv_new_connection_id_frame(const QuicNewConnectionIdFrame &frame) noexcept {
    // RFC 9000 §19.15: Connection ID length MUST be at least 1 and MUST NOT
    // exceed 20. retire_prior_to MUST be ≤ sequence_number.
    if (frame.cid_len == 0 || frame.cid_len > kMaxConnectionIdLength || frame.retire_prior_to > frame.sequence_number) {
        close(QuicErrorCode::FrameEncodingError, static_cast<std::uint64_t>(QuicFrameType::NewConnectionId));
        return std::unexpected(common::IoErr::Invalid);
    }

    auto incoming = QuicConnectionId::from_bytes(frame.cid, frame.cid_len);
    if (!incoming) {
        close(QuicErrorCode::FrameEncodingError, static_cast<std::uint64_t>(QuicFrameType::NewConnectionId));
        return std::unexpected(common::IoErr::Invalid);
    }
    const QuicConnectionId &new_cid = *incoming;

    bool send_output = false;

    // RFC 9000 §19.15:
    //   An endpoint that receives a NEW_CONNECTION_ID frame with a sequence
    //   number smaller than the Retire Prior To field of a previously received
    //   NEW_CONNECTION_ID frame MUST send a corresponding RETIRE_CONNECTION_ID
    //   frame.
    if (frame.sequence_number < max_retired_remote_seq_) {
        auto queued = queue_retire_connection_id_frame(frame.sequence_number);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        return true;
    }

    // De-duplicate: a retransmitted NEW_CONNECTION_ID with the same seqnum is
    // legal as long as the cid bytes and token match; otherwise § says we MAY
    // treat as PROTOCOL_VIOLATION.
    if (QuicRemoteConnectionIdSlot *existing = find_remote_connection_id_slot(frame.sequence_number)) {
        if (!connection_id_equal(existing->cid, new_cid) ||
            std::memcmp(existing->stateless_reset_token, frame.stateless_reset_token, kStatelessResetTokenLength) !=
                    0) {
            close(QuicErrorCode::ProtocolViolation, static_cast<std::uint64_t>(QuicFrameType::NewConnectionId));
            return std::unexpected(common::IoErr::Invalid);
        }
        // Already in pool: only retire_prior_to / limit checks remain.
    } else {
        QuicRemoteConnectionIdSlot *slot = find_free_remote_connection_id_slot();
        if (slot == nullptr) {
            // Over capacity even before applying retire_prior_to from this
            // very frame (which might free slots below). RFC 9000 §5.1.1
            // grants us at most active_connection_id_limit + 1 transient
            // entries — kQuicRemoteConnectionIdSlotCount is sized generously
            // so this is a hard limit error.
            close(QuicErrorCode::ConnectionIdLimitError, static_cast<std::uint64_t>(QuicFrameType::NewConnectionId));
            return std::unexpected(common::IoErr::Invalid);
        }
        slot->cid = new_cid;
        slot->sequence_number = frame.sequence_number;
        std::memcpy(slot->stateless_reset_token, frame.stateless_reset_token, kStatelessResetTokenLength);
        slot->in_use = true;
        slot->used = false;
        if (frame.sequence_number > largest_seen_remote_seq_) {
            largest_seen_remote_seq_ = frame.sequence_number;
        }
    }

    if (frame.retire_prior_to > max_retired_remote_seq_) {
        max_retired_remote_seq_ = frame.retire_prior_to;
        for (QuicRemoteConnectionIdSlot &slot: remote_cids_) {
            if (!slot.in_use || slot.sequence_number >= max_retired_remote_seq_) {
                continue;
            }
            auto retired = retire_remote_connection_id(slot);
            if (!retired) {
                return std::unexpected(retired.error());
            }
            send_output = send_output || *retired;
        }
    }

    // RFC 9000 §5.1.1: After processing NEW_CONNECTION_ID and adding/retiring
    // CIDs, if the number of active CIDs exceeds our advertised limit, close
    // the connection with CONNECTION_ID_LIMIT_ERROR.
    if (active_remote_connection_id_count() > options_.transport.active_connection_id_limit) {
        close(QuicErrorCode::ConnectionIdLimitError, static_cast<std::uint64_t>(QuicFrameType::NewConnectionId));
        return std::unexpected(common::IoErr::Invalid);
    }

    return send_output;
}

void QuicConnection::reset_congestion_for_path(QuicTime now) noexcept {
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    reset_packet_number_ = space.next_packet_number;
    quic_congestion_reset_for_path(congestion_, rtt_, now);
}


std::size_t QuicConnection::packet_number_space_index(QuicEncryptionLevel level) noexcept {
    switch (level) {
        case QuicEncryptionLevel::Initial:
            return 0;
        case QuicEncryptionLevel::Handshake:
            return 1;
        case QuicEncryptionLevel::EarlyData:
        case QuicEncryptionLevel::Application:
            return 2;
    }
    return 2;
}

std::uint8_t QuicConnection::local_initiator_bit() const noexcept {
    return options_.role == QuicConnectionRole::Server ? 1 : 0;
}

QuicConnection::PeerStreamLimitWindow &QuicConnection::peer_stream_window(QuicStreamType type) noexcept {
    return type == QuicStreamType::Bidirectional ? peer_bidi_streams_ : peer_uni_streams_;
}

const QuicConnection::PeerStreamLimitWindow &QuicConnection::peer_stream_window(QuicStreamType type) const noexcept {
    return type == QuicStreamType::Bidirectional ? peer_bidi_streams_ : peer_uni_streams_;
}

QuicConnection::LocalStreamBlockedState &QuicConnection::local_stream_blocked_state(QuicStreamType type) noexcept {
    return type == QuicStreamType::Bidirectional ? local_bidi_streams_blocked_ : local_uni_streams_blocked_;
}

const QuicConnection::LocalStreamBlockedState &
QuicConnection::local_stream_blocked_state(QuicStreamType type) const noexcept {
    return type == QuicStreamType::Bidirectional ? local_bidi_streams_blocked_ : local_uni_streams_blocked_;
}

std::uint64_t QuicConnection::local_stream_limit(QuicStreamType type) const noexcept {
    return type == QuicStreamType::Bidirectional ? options_.max_local_bidirectional_streams
                                                 : options_.max_local_unidirectional_streams;
}

std::uint64_t QuicConnection::peer_stream_limit(QuicStreamType type) const noexcept {
    return peer_stream_window(type).advertised_limit;
}

bool QuicConnection::local_stream_blocked(QuicStreamType type) const noexcept {
    const std::uint64_t next =
            type == QuicStreamType::Bidirectional ? next_local_bidi_stream_id_ : next_local_uni_stream_id_;
    return stream_sequence(next) >= local_stream_limit(type);
}

bool QuicConnection::local_stream_attach_ready(QuicStreamType type) const noexcept {
    return state_ == QuicConnectionState::Established && accepting_new_streams() && !local_stream_blocked(type);
}

void QuicConnection::wait_for_local_stream_attach(LocalStreamAttachAwaiter &awaiter) noexcept {
    common::IntrusiveListHook &hook = awaiter.wait_link_;
    if (hook.linked()) {
        return;
    }

    common::IntrusiveListHook *&head = awaiter.type_ == QuicStreamType::Bidirectional
                                               ? local_bidi_stream_attach_wait_head_
                                               : local_uni_stream_attach_wait_head_;
    common::IntrusiveListHook *&tail = awaiter.type_ == QuicStreamType::Bidirectional
                                               ? local_bidi_stream_attach_wait_tail_
                                               : local_uni_stream_attach_wait_tail_;

    hook.prev = tail;
    hook.next = nullptr;
    if (tail != nullptr) {
        tail->next = &hook;
    } else {
        head = &hook;
    }
    tail = &hook;
    hook.in_list = true;
}

void QuicConnection::cancel_local_stream_attach_wait(LocalStreamAttachAwaiter &awaiter) noexcept {
    common::IntrusiveListHook &hook = awaiter.wait_link_;
    if (!hook.linked()) {
        return;
    }

    common::IntrusiveListHook *&head = awaiter.type_ == QuicStreamType::Bidirectional
                                               ? local_bidi_stream_attach_wait_head_
                                               : local_uni_stream_attach_wait_head_;
    common::IntrusiveListHook *&tail = awaiter.type_ == QuicStreamType::Bidirectional
                                               ? local_bidi_stream_attach_wait_tail_
                                               : local_uni_stream_attach_wait_tail_;

    if (hook.prev != nullptr) {
        hook.prev->next = hook.next;
    } else {
        head = hook.next;
    }
    if (hook.next != nullptr) {
        hook.next->prev = hook.prev;
    } else {
        tail = hook.prev;
    }
    hook.prev = nullptr;
    hook.next = nullptr;
    hook.in_list = false;
}

void QuicConnection::notify_local_stream_attach_waiters(QuicStreamType type, common::IoErr result) noexcept {
    common::IntrusiveListHook *&head = type == QuicStreamType::Bidirectional ? local_bidi_stream_attach_wait_head_
                                                                             : local_uni_stream_attach_wait_head_;

    while (head != nullptr) {
        LocalStreamAttachAwaiter *awaiter = LocalStreamAttachAwaiter::from_wait_link(head);
        cancel_local_stream_attach_wait(*awaiter);
        awaiter->complete(result);
    }
}

void QuicConnection::notify_all_local_stream_attach_waiters(common::IoErr result) noexcept {
    notify_local_stream_attach_waiters(QuicStreamType::Bidirectional, result);
    notify_local_stream_attach_waiters(QuicStreamType::Unidirectional, result);
}

void QuicConnection::attach_to_endpoint() noexcept {
    FIBER_ASSERT(!attached_to_endpoint_);
    attached_to_endpoint_ = true;
    detached_from_endpoint_ = false;
}

void QuicConnection::detach_from_endpoint() noexcept {
    if (!attached_to_endpoint_ && detached_from_endpoint_) {
        return;
    }
    FIBER_ASSERT(state_ == QuicConnectionState::Closed);

    notify_all_local_stream_attach_waiters(common::IoErr::Canceled);
    notify_peer_data_waiters(common::IoErr::Canceled);
    close_all_streams(close_info_.error_code);
    clear_frames_for_detach();
    streams_.clear();

    options_.endpoint = nullptr;
    options_.schedule_send_owner = nullptr;
    options_.schedule_send = nullptr;
    options_.lifecycle_owner = nullptr;
    options_.on_idle_timeout = nullptr;
    options_.on_close_timeout = nullptr;

    endpoint_index.connection = nullptr;
    send_queue_entry.connection = nullptr;
    attached_to_endpoint_ = false;
    detached_from_endpoint_ = true;
}

void QuicConnection::retain() noexcept { ++ref_count_; }

void QuicConnection::release() noexcept {
    FIBER_ASSERT(ref_count_ > 0);
    --ref_count_;
    if (ready_for_destruction()) {
        FIBER_ASSERT(on_destroy_ != nullptr);
        on_destroy_(destroy_owner_, *this);
    }
}

bool QuicConnection::ready_for_destruction() const noexcept {
    return !attached_to_endpoint_ && ref_count_ == 0;
}

bool QuicConnection::can_queue_frame() const noexcept { return !detached_from_endpoint_; }

bool QuicConnection::is_gone_peer_stream(std::uint64_t stream_id) const noexcept {
    if (!is_peer_stream(stream_id)) {
        return false;
    }

    return stream_sequence(stream_id) < peer_stream_window(stream_type(stream_id)).opened_count;
}

bool QuicConnection::peer_stream_exceeds_advertised_limit(std::uint64_t stream_id) const noexcept {
    if (!is_peer_stream(stream_id)) {
        return false;
    }
    return stream_sequence(stream_id) >= peer_stream_window(stream_type(stream_id)).advertised_limit;
}

void QuicConnection::on_peer_stream_retired(std::uint64_t stream_id) noexcept {
    if (!is_peer_stream(stream_id)) {
        return;
    }

    const QuicStreamType type = stream_type(stream_id);
    PeerStreamLimitWindow &window = peer_stream_window(type);
    if (window.retired_count < window.opened_count) {
        ++window.retired_count;
    }
    maybe_extend_peer_stream_limit(type);
}

void QuicConnection::maybe_extend_peer_stream_limit(QuicStreamType type) noexcept {
    PeerStreamLimitWindow &window = peer_stream_window(type);
    if (window.concurrent_limit == 0 || window.advertised_limit >= kQuicMaxStreamLimit) {
        return;
    }

    std::uint64_t target = kQuicMaxStreamLimit;
    if (window.retired_count <= kQuicMaxStreamLimit &&
        window.concurrent_limit <= kQuicMaxStreamLimit - window.retired_count) {
        target = window.retired_count + window.concurrent_limit;
    }
    if (target <= window.advertised_limit) {
        return;
    }

    std::uint64_t threshold = window.concurrent_limit / 4;
    if (threshold == 0) {
        threshold = 1;
    }
    if (target != kQuicMaxStreamLimit && target - window.advertised_limit < threshold) {
        return;
    }

    auto queued = queue_max_streams_frame(type, target);
    if (queued) {
        window.advertised_limit = target;
    }
}

void QuicConnection::retire_stream(QuicStream &stream) noexcept {
    if (!stream.attached_to_connection()) {
        return;
    }

    const std::uint64_t stream_id = stream.stream_id();
    const bool peer_stream = is_peer_stream(stream_id);
    QuicStream::Lease lease = streams_.erase(stream.stream_id());
    if (!lease) {
        stream.detach_from_connection();
        // No further bookkeeping needed; if the stream wasn't in the table the
        // active count didn't change, so a graceful shutdown waiting on a count
        // transition to zero won't be falsely advanced.
        return;
    }
    if (peer_stream) {
        on_peer_stream_retired(stream_id);
    }
    lease->detach_from_connection();

    maybe_finish_graceful_close();
}

void QuicConnection::try_release_stream(QuicStream &stream) noexcept {
    if (stream.ready_for_connection_release()) {
        retire_stream(stream);
    }
}

void QuicConnection::drop_stream_send_ticket(std::uint64_t stream_id) noexcept {
    QuicStream *stream = find_stream(stream_id);
    if (stream == nullptr) {
        return;
    }
    stream->stream_send_pending_ = false;
    try_release_stream(*stream);
}

common::IoResult<void> QuicConnection::queue_stream_frame(QuicStream &stream) noexcept {
    if (!can_queue_frame() || closing() || !stream.attached_to_connection() || stream.stream_send_pending_ ||
        !stream.has_send_work()) {
        return {};
    }

    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::Stream;
    frame->u.stream.stream_id = stream.stream_id();
    stream.stream_send_pending_ = true;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

void QuicConnection::schedule_send() noexcept {
    if (!can_queue_frame()) {
        return;
    }
    if (options_.schedule_send != nullptr) {
        options_.schedule_send(options_.schedule_send_owner, *this);
    }
}

common::IoResult<void> QuicConnection::queue_ping_frame() noexcept {
    if (!can_queue_frame() || closing()) {
        return {};
    }

    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::Ping;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

bool QuicConnection::has_pending_send_work() const noexcept {
    if (!can_queue_frame()) {
        return false;
    }
    if (has_path_send_work()) {
        return true;
    }
    for (const QuicPacketNumberSpace &space: packet_number_spaces_) {
        if ((space.send_ack && space.pending_ack != kUnsetPacketNumber) || !space.pending_frames.empty()) {
            return true;
        }
    }
    return false;
}


common::IoResult<void> QuicConnection::on_stream_send_acked(std::uint64_t stream_id, std::size_t offset,
                                                            std::size_t length, bool fin) noexcept {
    QuicStream *stream = find_stream(stream_id);
    if (stream == nullptr) {
        return {};
    }
    auto acked = stream->mark_send_acked(offset, length, fin);
    if (!acked) {
        return std::unexpected(acked.error());
    }
    try_release_stream(*stream);
    return {};
}

common::IoResult<void> QuicConnection::on_stream_send_failed(std::uint64_t stream_id, std::size_t offset,
                                                             std::size_t length, bool fin) noexcept {
    QuicStream *stream = find_stream(stream_id);
    if (stream == nullptr) {
        return {};
    }
    auto failed = stream->mark_send_failed(offset, length, fin);
    if (!failed) {
        return std::unexpected(failed.error());
    }
    return queue_stream_frame(*stream);
}

common::IoResult<void> QuicConnection::queue_reset_stream_frame(std::uint64_t stream_id, std::uint64_t error_code,
                                                                std::uint64_t final_size) noexcept {
    if (!can_queue_frame() || terminal_closing()) {
        return {};
    }
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::ResetStream;
    frame->u.reset_stream.id = stream_id;
    frame->u.reset_stream.error_code = error_code;
    frame->u.reset_stream.final_size = final_size;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_stop_sending_frame(std::uint64_t stream_id,
                                                                std::uint64_t error_code) noexcept {
    if (!can_queue_frame() || terminal_closing()) {
        return {};
    }
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::StopSending;
    frame->u.stop_sending.id = stream_id;
    frame->u.stop_sending.error_code = error_code;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_max_stream_data_frame(std::uint64_t stream_id,
                                                                   std::uint64_t limit) noexcept {
    if (!can_queue_frame()) {
        return {};
    }
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::MaxStreamData;
    frame->u.max_stream_data.id = stream_id;
    frame->u.max_stream_data.limit = limit;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_max_streams_frame(QuicStreamType type, std::uint64_t limit) noexcept {
    if (!can_queue_frame() || closing()) {
        return {};
    }
    if (limit > kQuicMaxStreamLimit) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = type == QuicStreamType::Bidirectional ? QuicFrameType::MaxStreamsBidi : QuicFrameType::MaxStreamsUni;
    frame->u.max_streams.limit = limit;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_max_data_frame(std::uint64_t limit) noexcept {
    if (!can_queue_frame()) {
        return {};
    }
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::MaxData;
    frame->u.max_data.max_data = limit;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_streams_blocked_frame(QuicStreamType type, std::uint64_t limit) noexcept {
    if (!can_queue_frame() || closing()) {
        return {};
    }
    if (limit > kQuicMaxStreamLimit) {
        return std::unexpected(common::IoErr::Invalid);
    }

    LocalStreamBlockedState &blocked = local_stream_blocked_state(type);
    if (blocked.reported && blocked.last_limit == limit) {
        return {};
    }

    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = type == QuicStreamType::Bidirectional ? QuicFrameType::StreamsBlockedBidi
                                                        : QuicFrameType::StreamsBlockedUni;
    frame->u.streams_blocked.limit = limit;
    space.pending_frames.push_back(*frame);
    blocked.reported = true;
    blocked.last_limit = limit;
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_data_blocked_frame(std::uint64_t limit) noexcept {
    if (!can_queue_frame() || closing() || (data_blocked_reported_ && last_data_blocked_limit_ == limit)) {
        return {};
    }

    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::DataBlocked;
    frame->u.data_blocked.limit = limit;
    space.pending_frames.push_back(*frame);
    data_blocked_reported_ = true;
    last_data_blocked_limit_ = limit;
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_stream_data_blocked_frame(QuicStream &stream,
                                                                       std::uint64_t limit) noexcept {
    if (!can_queue_frame() || closing() || !stream.attached_to_connection() ||
        (stream.stream_data_blocked_reported_ && stream.last_stream_data_blocked_limit_ == limit)) {
        return {};
    }

    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::StreamDataBlocked;
    frame->u.stream_data_blocked.id = stream.stream_id();
    frame->u.stream_data_blocked.limit = limit;
    space.pending_frames.push_back(*frame);
    stream.stream_data_blocked_reported_ = true;
    stream.last_stream_data_blocked_limit_ = limit;
    schedule_send();
    return {};
}

std::uint64_t QuicConnection::peer_data_available() const noexcept {
    if (peer_data_reserved_ >= peer_max_data_) {
        return 0;
    }
    return peer_max_data_ - peer_data_reserved_;
}

bool QuicConnection::should_retransmit_data_blocked(std::uint64_t limit) const noexcept {
    return !closing() && peer_max_data_ == limit && peer_data_available() == 0;
}

bool QuicConnection::should_retransmit_stream_data_blocked(std::uint64_t stream_id,
                                                           std::uint64_t limit) const noexcept {
    const QuicStream *stream = find_stream(stream_id);
    return stream != nullptr && !closing() && stream->should_retransmit_stream_data_blocked(limit);
}

bool QuicConnection::should_retransmit_max_streams(QuicStreamType type, std::uint64_t limit) const noexcept {
    return !closing() && peer_stream_window(type).advertised_limit == limit;
}

bool QuicConnection::should_retransmit_streams_blocked(QuicStreamType type, std::uint64_t limit) const noexcept {
    return !closing() && local_stream_limit(type) == limit && local_stream_blocked(type);
}

bool QuicConnection::reserve_peer_data(std::uint64_t bytes) noexcept {
    if (bytes == 0) {
        return true;
    }
    if (bytes > peer_data_available()) {
        return false;
    }
    peer_data_reserved_ += bytes;
    return true;
}

std::uint64_t QuicConnection::initial_stream_send_limit(std::uint64_t stream_id) const noexcept {
    if (!peer_transport_.received) {
        return 0;
    }
    const QuicStreamType type = stream_type(stream_id);
    if (type == QuicStreamType::Unidirectional) {
        return is_local_stream(stream_id) ? peer_transport_.params.initial_max_stream_data_uni : 0;
    }
    return is_local_stream(stream_id) ? peer_transport_.params.initial_max_stream_data_bidi_remote
                                      : peer_transport_.params.initial_max_stream_data_bidi_local;
}

common::IoResult<void> QuicConnection::check_recv_data_delta(std::uint64_t delta) const noexcept {
    if (delta > recv_data_limit_ || recv_data_consumed_ > recv_data_limit_ - delta) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    return {};
}

void QuicConnection::commit_recv_data_delta(std::uint64_t delta) noexcept { recv_data_consumed_ += delta; }

void QuicConnection::maybe_extend_recv_data_flow_control() noexcept {
    const std::uint64_t window = options_.recv_flow.conn_recv_limit;
    if (window == 0) {
        return;
    }

    const std::uint64_t remaining = recv_data_limit_ > recv_data_consumed_ ? recv_data_limit_ - recv_data_consumed_ : 0;
    if (remaining >= options_.recv_flow.conn_recv_low_water) {
        return;
    }

    std::uint64_t limit = kMaxVarint;
    if (recv_data_limit_ <= kMaxVarint && window <= kMaxVarint - recv_data_limit_) {
        limit = recv_data_limit_ + window;
    }
    if (limit <= recv_data_limit_) {
        return;
    }

    auto queued = queue_max_data_frame(limit);
    if (queued) {
        recv_data_limit_ = limit;
    }
}

} // namespace fiber::quic
