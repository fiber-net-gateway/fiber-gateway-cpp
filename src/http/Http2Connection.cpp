#include "Http2Connection.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

#include "../common/Assert.h"

namespace fiber::http {

namespace {

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kFrameHeaderSize = 9;
constexpr std::size_t kSettingsParameterSize = 6;
constexpr std::size_t kPingPayloadSize = 8;
constexpr std::size_t kWindowUpdatePayloadSize = 4;
constexpr std::size_t kRstStreamPayloadSize = 4;
constexpr std::size_t kGoawayMinimumPayloadSize = 8;
constexpr std::uint16_t kSettingsHeaderTableSize = 0x1;
constexpr std::uint16_t kSettingsEnablePush = 0x2;
constexpr std::uint16_t kSettingsMaxConcurrentStreams = 0x3;
constexpr std::uint16_t kSettingsInitialWindowSize = 0x4;
constexpr std::uint16_t kSettingsMaxFrameSize = 0x5;
constexpr std::uint16_t kSettingsMaxHeaderListSize = 0x6;
constexpr std::uint16_t kSettingsEnableConnectProtocol = 0x8;
constexpr std::uint8_t kFlagAck = 0x1;
constexpr std::uint8_t kFlagSettingsAck = 0x1;
constexpr std::uint8_t kFlagEndStream = 0x1;
constexpr std::uint8_t kFlagEndHeaders = 0x4;
constexpr std::uint8_t kFlagPadded = 0x8;
constexpr std::uint8_t kFlagPriority = 0x20;
constexpr std::int32_t kDefaultInitialWindowSize = 65535;
constexpr std::uint32_t kDefaultHeaderTableSize = 4096;
constexpr std::uint32_t kDefaultMaxFrameSize = 16384;
constexpr std::uint32_t kMaxFrameSizeLimit = 16777215;
constexpr std::int64_t kMaxFlowControlWindow = 0x7fffffffLL;
constexpr std::int32_t kInitialFlowControlWindow = 65535;
constexpr std::size_t kIoPumpOperationBudget = 64;
constexpr std::size_t kIoPumpByteBudget = 256 * 1024;

using TimePoint = std::chrono::steady_clock::time_point;

TimePoint deadline_after(TimePoint now, std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return TimePoint::max();
    }
    return now + timeout;
}

std::uint32_t parse_frame_length(const std::uint8_t *pos) noexcept {
    return (static_cast<std::uint32_t>(pos[0]) << 16) | (static_cast<std::uint32_t>(pos[1]) << 8) |
           static_cast<std::uint32_t>(pos[2]);
}

std::uint32_t parse_stream_id(const std::uint8_t *pos) noexcept {
    return ((static_cast<std::uint32_t>(pos[0]) & 0x7fU) << 24) | (static_cast<std::uint32_t>(pos[1]) << 16) |
           (static_cast<std::uint32_t>(pos[2]) << 8) | static_cast<std::uint32_t>(pos[3]);
}

std::uint32_t parse_u32(const std::uint8_t *pos) noexcept {
    return (static_cast<std::uint32_t>(pos[0]) << 24) | (static_cast<std::uint32_t>(pos[1]) << 16) |
           (static_cast<std::uint32_t>(pos[2]) << 8) | static_cast<std::uint32_t>(pos[3]);
}

std::uint16_t parse_u16(const std::uint8_t *pos) noexcept {
    return (static_cast<std::uint16_t>(pos[0]) << 8) | static_cast<std::uint16_t>(pos[1]);
}

std::uint8_t *append_u16(std::uint8_t *out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[1] = static_cast<std::uint8_t>(value & 0xffU);
    return out + 2;
}

std::uint8_t *append_u32(std::uint8_t *out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[3] = static_cast<std::uint8_t>(value & 0xffU);
    return out + 4;
}

std::uint8_t *append_u64(std::uint8_t *out, std::uint64_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 56) & 0xffU);
    out[1] = static_cast<std::uint8_t>((value >> 48) & 0xffU);
    out[2] = static_cast<std::uint8_t>((value >> 40) & 0xffU);
    out[3] = static_cast<std::uint8_t>((value >> 32) & 0xffU);
    out[4] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    out[5] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    out[6] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[7] = static_cast<std::uint8_t>(value & 0xffU);
    return out + 8;
}

common::IoErr prepare_read_buffer(mem::IoBuf &read_buf, std::size_t capacity) noexcept {
    if (!read_buf) {
        read_buf = mem::IoBuf::allocate(capacity);
        return read_buf ? common::IoErr::None : common::IoErr::NoMem;
    }

    std::size_t unread = read_buf.readable();
    const std::uint8_t *unread_begin = read_buf.readable_data();

    if (!read_buf.unique()) {
        mem::IoBuf next = mem::IoBuf::allocate(capacity);
        if (!next) {
            return common::IoErr::NoMem;
        }
        if (unread != 0) {
            std::memcpy(next.writable_data(), unread_begin, unread);
            next.commit(unread);
        }
        read_buf = std::move(next);
        return common::IoErr::None;
    }

    if (unread == 0) {
        read_buf.clear();
        return common::IoErr::None;
    }

    if (unread_begin != read_buf.data()) {
        std::memmove(read_buf.data(), unread_begin, unread);
    }
    read_buf.clear();
    read_buf.commit(unread);
    return common::IoErr::None;
}

} // namespace

Http2Connection::Http2Connection(Options options, void *peer_stream_factory_ctx,
                                 const Http2StreamFactoryOps &peer_stream_factory_ops) :
    options_(std::move(options)), peer_stream_factory_ctx_(peer_stream_factory_ctx),
    peer_stream_factory_ops_(peer_stream_factory_ops) {
    FIBER_ASSERT(peer_stream_factory_ops_.create_peer_stream != nullptr);
    peer_advertised_max_concurrent_streams_ = options_.max_peer_concurrent_streams;
    peer_initial_stream_send_window_ = options_.initial_stream_send_window;
    conn_recv_window_target_ =
            std::max(options_.initial_connection_recv_window, static_cast<std::uint32_t>(kInitialFlowControlWindow));
    conn_recv_window_remaining_ = static_cast<std::int32_t>(conn_recv_window_target_);
    next_local_stream_id_ = options_.role == ConnectionRole::Client ? 1U : 2U;
    peer_max_outbound_frame_size_ = options_.max_frame_size;
    conn_send_window_ = static_cast<std::int32_t>(options_.initial_connection_send_window);
    FIBER_ASSERT(streams_.init(configured_max_active_streams()));
    FIBER_ASSERT(inbound_hpack_decoder_.init(kDefaultHeaderTableSize, options_.max_hpack_string_size));
}

common::IoErr Http2Connection::start(std::unique_ptr<HttpTransport> transport, ClosedCallback on_closed,
                                     void *closed_ctx) noexcept {
    FIBER_ASSERT(state_ == State::Init);
    FIBER_ASSERT(transport_ == nullptr);
    if (!transport || !transport->valid()) {
        return common::IoErr::Invalid;
    }
    if (!transport->loop().in_loop()) {
        return common::IoErr::NotSupported;
    }

    transport_ = std::move(transport);
    bind_outbound_chain(control_hook_.encoded_);
    bind_outbound_chain(inflight_outbound_chain_);
    on_closed_ = on_closed;
    closed_ctx_ = on_closed ? closed_ctx : nullptr;
    inbound_io_.phase = options_.role == ConnectionRole::Server ? ParsePhase::Preface : ParsePhase::FrameHeader;
    inbound_io_.last_inbound_at = transport_->loop().now();
    prefer_write_ = options_.role == ConnectionRole::Client;
    const common::IoErr start_err =
            options_.role == ConnectionRole::Client ? start_client_session() : start_server_session();
    if (start_err != common::IoErr::None) {
        terminal_error_ = start_err;
        abort_outbound(start_err);
        transport_->close();
        state_ = State::Closed;
        close_finished_ = true;
        close_completion_dispatched_ = true;
        on_closed_ = nullptr;
        closed_ctx_ = nullptr;
        return start_err;
    }

    schedule_io_pump();
    return common::IoErr::None;
}

Http2Connection::~Http2Connection() {
    FIBER_ASSERT(!io_pump_running_);
    if (io_pump_posted_ && transport_ && transport_->loop().in_loop()) {
        transport_->loop().cancel<Http2Connection, &Http2Connection::io_pump_entry_>(*this);
        io_pump_posted_ = false;
    }
    if (close_completion_posted_ && transport_ && transport_->loop().in_loop()) {
        transport_->loop().cancel<Http2Connection, &Http2Connection::close_completion_entry_>(*this);
        close_completion_posted_ = false;
    }
    FIBER_ASSERT(!io_pump_posted_);
    on_closed_ = nullptr;
    closed_ctx_ = nullptr;
    if (transport_ && transport_->loop().in_loop()) {
        cancel_io_timers();
        clear_transport_callbacks();
    }
    clear_inbound_stream();
    state_ = State::Closed;
    stop_sending_requested_ = true;
    abort_outbound(common::IoErr::Canceled);
    close_all_streams(common::IoErr::Canceled);
    while (Http2Stream *stream = owned_stream_list_.front()) {
        std::uint32_t stream_id = stream->stream_id_;
        owned_stream_list_.erase(*stream);
        stream->attached_to_connection_ = false;
        stream->conn_ = nullptr;
        stream->active_ = false;
        (void) streams_.erase(stream_id);
    }
    inbound_io_.read_buf = {};
    if (transport_ && transport_->valid()) {
        transport_->close();
    }
    close_finished_ = true;
    FIBER_ASSERT(!closed_waiter_);
    FIBER_ASSERT(!close_completion_posted_);
}

fiber::async::Task<Http2Connection::RunResult> Http2Connection::run() noexcept { co_return co_await wait_closed(); }

fiber::async::Task<Http2Connection::RunResult> Http2Connection::wait_closed() noexcept {
    if (state_ == State::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (closed_waiter_ || on_closed_) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    co_await ClosedAwaiter{this};
    if (terminal_error_ != common::IoErr::None) {
        co_return std::unexpected(terminal_error_);
    }
    co_return RunResult{};
}

Http2Connection::ClosedAwaiter::~ClosedAwaiter() {
    if (connection && handle && connection->closed_waiter_ == handle) {
        connection->closed_waiter_ = {};
    }
}

bool Http2Connection::ClosedAwaiter::await_suspend(std::coroutine_handle<> continuation) noexcept {
    FIBER_ASSERT(connection != nullptr);
    FIBER_ASSERT(!connection->closed_waiter_);
    if (connection->close_completion_dispatched_) {
        return false;
    }
    handle = continuation;
    connection->closed_waiter_ = continuation;
    return true;
}

void Http2Connection::ClosedAwaiter::await_resume() noexcept {
    connection = nullptr;
    handle = {};
}

common::IoErr Http2Connection::consume_read_buffer(std::size_t &operation_budget, std::size_t &byte_budget) noexcept {
    mem::IoBuf &read_buf = inbound_io_.read_buf;
    while (operation_budget != 0 && byte_budget != 0) {
        if (inbound_io_.phase == ParsePhase::Preface) {
            if (read_buf.readable() < kClientPreface.size()) {
                return common::IoErr::None;
            }
            if (std::memcmp(read_buf.readable_data(), kClientPreface.data(), kClientPreface.size()) != 0) {
                return common::IoErr::Invalid;
            }
            read_buf.consume(kClientPreface.size());
            --operation_budget;
            byte_budget -= std::min(byte_budget, kClientPreface.size());
            common::IoErr err = send_initial_flight();
            if (err != common::IoErr::None) {
                return err;
            }
            state_ = State::Running;
            inbound_io_.phase = ParsePhase::FrameHeader;
            continue;
        }

        if (inbound_io_.phase == ParsePhase::FrameHeader) {
            if (read_buf.readable() < kFrameHeaderSize) {
                return common::IoErr::None;
            }

            const std::uint8_t *header = read_buf.readable_data();
            FrameHeader &current_header = inbound_io_.current_header;
            current_header.length = parse_frame_length(header);
            current_header.type = static_cast<Http2FrameType>(header[3]);
            current_header.flags = header[4];
            current_header.stream_id = parse_stream_id(header + 5);
            if (current_header.length > options_.max_frame_size) {
                return common::IoErr::Invalid;
            }

            read_buf.consume(kFrameHeaderSize);
            --operation_budget;
            byte_budget -= std::min(byte_budget, kFrameHeaderSize);
            inbound_io_.payload_remaining = current_header.length;
            inbound_io_.payload_offset = 0;
            if (inbound_io_.payload_remaining == 0) {
                common::IoErr err = consume_incoming_frame_payload(current_header, read_buf, 0, 0);
                if (err != common::IoErr::None || state_ == State::Closing || state_ == State::Closed) {
                    return err;
                }
                continue;
            }
            inbound_io_.phase = ParsePhase::FramePayload;
            continue;
        }

        if (read_buf.readable() == 0) {
            return common::IoErr::None;
        }

        const std::size_t chunk_len =
                std::min({read_buf.readable(), static_cast<std::size_t>(inbound_io_.payload_remaining), byte_budget});
        common::IoErr err = consume_incoming_frame_payload(inbound_io_.current_header, read_buf,
                                                           inbound_io_.payload_offset, chunk_len);
        if (err != common::IoErr::None) {
            return err;
        }
        read_buf.consume(chunk_len);
        inbound_io_.payload_remaining -= static_cast<std::uint32_t>(chunk_len);
        inbound_io_.payload_offset += chunk_len;
        --operation_budget;
        byte_budget -= chunk_len;
        if (inbound_io_.payload_remaining == 0) {
            inbound_io_.phase = ParsePhase::FrameHeader;
        }
        if (state_ == State::Closing || state_ == State::Closed) {
            return common::IoErr::None;
        }
    }
    return common::IoErr::None;
}

common::IoResult<Http2Connection::ReadPumpResult> Http2Connection::pump_read(std::size_t operation_budget,
                                                                             std::size_t byte_budget) noexcept {
    operation_budget = std::max<std::size_t>(operation_budget, 1);
    byte_budget = std::max<std::size_t>(byte_budget, 1);
    ReadPumpResult result;
    if (inbound_eof_) {
        return result;
    }
    const std::size_t read_buffer_capacity = std::max(options_.read_buffer_size, kClientPreface.size());

    while (state_ == State::Start || state_ == State::Running || state_ == State::Draining) {
        common::IoErr consume_err = consume_read_buffer(operation_budget, byte_budget);
        if (consume_err != common::IoErr::None) {
            return std::unexpected(consume_err);
        }
        if (state_ == State::Closing || state_ == State::Closed) {
            return result;
        }
        if (operation_budget == 0 || byte_budget == 0) {
            result.needs_reschedule = true;
            return result;
        }

        const bool buffered_read_ready = transport_->has_pending_read();
        if (!inbound_io_.ready_hint && !buffered_read_ready) {
            result.wait_event = inbound_io_.operation_pending ? inbound_io_.wait_event : event::IoEvent::Read;
            return result;
        }
        common::IoErr prepare_err = prepare_read_buffer(inbound_io_.read_buf, read_buffer_capacity);
        if (prepare_err != common::IoErr::None) {
            return std::unexpected(prepare_err);
        }

        std::size_t bytes_read = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr read_err = transport_->poll_read_into(inbound_io_.read_buf, bytes_read, wait_event);
        inbound_io_.ready_hint = false;
        --operation_budget;
        if (read_err == common::IoErr::WouldBlock) {
            if (wait_event != event::IoEvent::Read && wait_event != event::IoEvent::Write) {
                return std::unexpected(common::IoErr::Invalid);
            }
            inbound_io_.operation_pending = true;
            result.wait_event = wait_event;
            return result;
        }
        inbound_io_.operation_pending = false;
        if (read_err != common::IoErr::None) {
            return std::unexpected(read_err);
        }
        if (bytes_read == 0) {
            handle_read_eof();
            return result;
        }

        // A readiness notification permits draining the nonblocking transport
        // until it reports WouldBlock. This also consumes any TLS plaintext
        // that became available from the same socket event without waiting for
        // another physical readiness edge.
        inbound_io_.ready_hint = true;
        result.bytes_read += bytes_read;
        byte_budget -= std::min(byte_budget, bytes_read);
        inbound_io_.last_inbound_at = transport_->loop().now();
        if (operation_budget == 0 || byte_budget == 0) {
            result.needs_reschedule = true;
            return result;
        }
    }
    return result;
}

void Http2Connection::handle_read_eof() noexcept {
    if (inbound_io_.phase != ParsePhase::FrameHeader || inbound_io_.read_buf.readable() != 0) {
        enter_closing(common::IoErr::ConnReset);
        return;
    }
    if (state_ == State::Closed || state_ == State::Closing || inbound_eof_) {
        return;
    }
    inbound_eof_ = true;
    inbound_io_.operation_pending = false;
    inbound_io_.wait_event = event::IoEvent::None;
    state_ = State::Closing;
    close_flush_outbound_ = true;
    close_outbound();
}

void Http2Connection::on_transport_read_ready(void *ctx, common::IoErr err) noexcept {
    auto *connection = static_cast<Http2Connection *>(ctx);
    FIBER_ASSERT(connection != nullptr);
    connection->handle_transport_ready(event::IoEvent::Read, err);
}

void Http2Connection::on_transport_write_ready(void *ctx, common::IoErr err) noexcept {
    auto *connection = static_cast<Http2Connection *>(ctx);
    FIBER_ASSERT(connection != nullptr);
    connection->handle_transport_ready(event::IoEvent::Write, err);
}

void Http2Connection::on_io_pump(Http2Connection *connection) noexcept {
    FIBER_ASSERT(connection != nullptr);
    connection->io_pump_posted_ = false;
    connection->drive_io();
}

void Http2Connection::handle_transport_ready(event::IoEvent event, common::IoErr err) noexcept {
    if (state_ == State::Closed) {
        return;
    }
    if (err != common::IoErr::None) {
        if (state_ == State::Closing && err == common::IoErr::Canceled) {
            return;
        }
        enter_closing(err);
        return;
    }
    const bool wakes_inbound = event::any(inbound_io_.wait_event & event);
    const bool wakes_outbound = event::any(outbound_wait_event_ & event);
    inbound_io_.ready_hint = inbound_io_.ready_hint || wakes_inbound;
    outbound_ready_hint_ = outbound_ready_hint_ || wakes_outbound;
    if (wakes_inbound != wakes_outbound) {
        prefer_write_ = wakes_outbound;
    }
    drive_io();
}

void Http2Connection::schedule_io_pump() noexcept {
    if (!transport_ || state_ == State::Closed) {
        return;
    }
    if (io_pump_running_) {
        io_pump_again_ = true;
        return;
    }
    if (io_pump_posted_) {
        return;
    }
    FIBER_ASSERT(transport_->loop().in_loop());
    io_pump_posted_ = true;
    transport_->loop().post_local<Http2Connection, &Http2Connection::io_pump_entry_, &Http2Connection::on_io_pump>(
            *this);
}

void Http2Connection::drive_io() noexcept {
    if (!transport_ || state_ == State::Closed) {
        return;
    }
    FIBER_ASSERT(transport_->loop().in_loop());
    if (io_pump_running_) {
        io_pump_again_ = true;
        return;
    }

    io_pump_running_ = true;
    io_pump_again_ = false;
    ReadPumpResult read_result;
    OutboundPumpResult write_result;
    bool write_progress = false;

    auto pump_inbound = [&]() noexcept -> bool {
        if (state_ != State::Start && state_ != State::Running && state_ != State::Draining) {
            inbound_io_.wait_event = event::IoEvent::None;
            return true;
        }
        auto result = pump_read(kIoPumpOperationBudget, kIoPumpByteBudget);
        if (!result) {
            enter_closing(result.error());
            return false;
        }
        read_result = *result;
        inbound_io_.wait_event = read_result.wait_event;
        return state_ != State::Closed;
    };

    auto pump_outbound = [&]() noexcept -> bool {
        if (outbound_stopped_) {
            outbound_wait_event_ = event::IoEvent::None;
            outbound_ready_hint_ = false;
            return true;
        }
        if (outbound_wait_event_ != event::IoEvent::None && !outbound_ready_hint_) {
            return true;
        }
        outbound_ready_hint_ = false;
        auto result = this->pump_outbound(kIoPumpOperationBudget, kIoPumpByteBudget);
        if (!result) {
            enter_closing(result.error());
            return false;
        }
        write_result = *result;
        write_progress = write_result.bytes_written != 0;
        outbound_wait_event_ = write_result.wait_event;
        return state_ != State::Closed;
    };

    if (prefer_write_) {
        (void) pump_outbound();
        if (state_ != State::Closed) {
            (void) pump_inbound();
        }
    } else {
        (void) pump_inbound();
        if (state_ != State::Closed) {
            (void) pump_outbound();
        }
    }
    prefer_write_ = !prefer_write_;

    if (state_ != State::Closed) {
        common::IoErr callback_err = sync_transport_callbacks();
        if (callback_err != common::IoErr::None) {
            enter_closing(callback_err);
        }
    }
    if (state_ != State::Closed) {
        arm_read_timer();
        arm_write_timer(write_progress);
        arm_read_buffer_idle_timer();
        if (read_result.needs_reschedule || write_result.needs_reschedule || io_pump_again_) {
            io_pump_again_ = true;
        }
        if (state_ == State::Closing) {
            finish_connection();
        }
    }

    io_pump_running_ = false;
    if (state_ == State::Closed) {
        schedule_closed_completion();
        return;
    }
    if (state_ != State::Closed && io_pump_again_) {
        io_pump_again_ = false;
        schedule_io_pump();
    }
}

common::IoErr Http2Connection::sync_transport_callbacks() noexcept {
    event::IoEvent wanted = inbound_io_.wait_event | outbound_wait_event_;
    const bool want_read = event::any(wanted & event::IoEvent::Read);
    const bool want_write = event::any(wanted & event::IoEvent::Write);

    if (want_read && !physical_read_registered_) {
        common::IoErr err = transport_->set_read_callback(&Http2Connection::on_transport_read_ready, this);
        if (err != common::IoErr::None) {
            return err;
        }
        physical_read_registered_ = true;
    }
    if (want_write && !physical_write_registered_) {
        common::IoErr err = transport_->set_write_callback(&Http2Connection::on_transport_write_ready, this);
        if (err != common::IoErr::None) {
            return err;
        }
        physical_write_registered_ = true;
    }
    if (!want_read && physical_read_registered_) {
        common::IoErr err = transport_->clear_read_callback(&Http2Connection::on_transport_read_ready, this);
        if (err != common::IoErr::None) {
            return err;
        }
        physical_read_registered_ = false;
    }
    if (!want_write && physical_write_registered_) {
        common::IoErr err = transport_->clear_write_callback(&Http2Connection::on_transport_write_ready, this);
        if (err != common::IoErr::None) {
            return err;
        }
        physical_write_registered_ = false;
    }
    return common::IoErr::None;
}

void Http2Connection::clear_transport_callbacks() noexcept {
    if (!transport_) {
        return;
    }
    if (physical_read_registered_) {
        (void) transport_->clear_read_callback(&Http2Connection::on_transport_read_ready, this);
        physical_read_registered_ = false;
    }
    if (physical_write_registered_) {
        (void) transport_->clear_write_callback(&Http2Connection::on_transport_write_ready, this);
        physical_write_registered_ = false;
    }
}

void Http2Connection::on_read_timer(Http2Connection *connection) noexcept {
    FIBER_ASSERT(connection != nullptr);
    common::IoErr err = connection->handle_read_timeout();
    if (err != common::IoErr::None) {
        connection->enter_closing(err);
        return;
    }
    connection->inbound_io_.ping_sent_at = connection->transport_->loop().now();
    connection->arm_read_timer();
    connection->schedule_io_pump();
}

void Http2Connection::on_write_timer(Http2Connection *connection) noexcept {
    FIBER_ASSERT(connection != nullptr);
    connection->enter_closing(common::IoErr::TimedOut);
}

void Http2Connection::on_read_buffer_idle_timer(Http2Connection *connection) noexcept {
    FIBER_ASSERT(connection != nullptr);
    const bool retry_buffer_pinned =
            connection->inbound_io_.operation_pending && connection->transport_->requires_stable_read_buffer_on_retry();
    if (!retry_buffer_pinned && connection->inbound_io_.read_buf && connection->inbound_io_.read_buf.unique() &&
        connection->inbound_io_.read_buf.readable() == 0) {
        connection->inbound_io_.read_buf = {};
    }
    if (connection->transport_ && connection->transport_->has_pending_read()) {
        connection->schedule_io_pump();
    }
}

void Http2Connection::arm_read_timer() noexcept {
    if (!transport_) {
        return;
    }
    event::EventLoop &event_loop = transport_->loop();
    event_loop.cancel<Http2Connection, &Http2Connection::read_timer_entry_>(*this);
    if (inbound_eof_ || (state_ != State::Start && state_ != State::Running && state_ != State::Draining)) {
        return;
    }
    const std::chrono::milliseconds timeout = current_read_timeout();
    if (timeout == std::chrono::milliseconds::max()) {
        return;
    }
    TimePoint base = keepalive_ping_outstanding_ ? inbound_io_.ping_sent_at : inbound_io_.last_inbound_at;
    if (base == TimePoint{}) {
        base = event_loop.now();
    }
    event_loop.post_at<Http2Connection, &Http2Connection::read_timer_entry_, &Http2Connection::on_read_timer>(
            deadline_after(base, timeout), *this);
}

void Http2Connection::arm_write_timer(bool made_progress) noexcept {
    if (!transport_) {
        return;
    }
    event::EventLoop &event_loop = transport_->loop();
    event_loop.cancel<Http2Connection, &Http2Connection::write_timer_entry_>(*this);
    if (outbound_wait_event_ == event::IoEvent::None || options_.write_timeout == std::chrono::milliseconds::max() ||
        state_ == State::Closed) {
        write_blocked_at_ = {};
        return;
    }
    if (made_progress || write_blocked_at_ == TimePoint{}) {
        write_blocked_at_ = event_loop.now();
    }
    event_loop.post_at<Http2Connection, &Http2Connection::write_timer_entry_, &Http2Connection::on_write_timer>(
            deadline_after(write_blocked_at_, options_.write_timeout), *this);
}

void Http2Connection::arm_read_buffer_idle_timer() noexcept {
    if (!transport_) {
        return;
    }
    event::EventLoop &event_loop = transport_->loop();
    event_loop.cancel<Http2Connection, &Http2Connection::read_buffer_idle_timer_entry_>(*this);
    const bool retry_buffer_pinned =
            inbound_io_.operation_pending && transport_->requires_stable_read_buffer_on_retry();
    if (options_.read_buffer_idle_release_timeout <= std::chrono::milliseconds::zero() || !inbound_io_.read_buf ||
        !inbound_io_.read_buf.unique() || inbound_io_.read_buf.readable() != 0 || retry_buffer_pinned ||
        state_ == State::Closed) {
        return;
    }
    event_loop.post_at<Http2Connection, &Http2Connection::read_buffer_idle_timer_entry_,
                       &Http2Connection::on_read_buffer_idle_timer>(
            deadline_after(inbound_io_.last_inbound_at, options_.read_buffer_idle_release_timeout), *this);
}

void Http2Connection::cancel_io_timers() noexcept {
    if (!transport_) {
        return;
    }
    event::EventLoop &event_loop = transport_->loop();
    event_loop.cancel<Http2Connection, &Http2Connection::read_timer_entry_>(*this);
    event_loop.cancel<Http2Connection, &Http2Connection::write_timer_entry_>(*this);
    event_loop.cancel<Http2Connection, &Http2Connection::read_buffer_idle_timer_entry_>(*this);
}

void Http2Connection::finish_connection() noexcept {
    if (state_ == State::Closed) {
        return;
    }
    if (state_ != State::Closing) {
        return;
    }
    if (close_flush_outbound_ && !outbound_stopped_) {
        return;
    }

    if (!close_finished_) {
        close_finished_ = true;
        clear_inbound_stream();
        inbound_io_.read_buf = {};
        const common::IoErr stream_close_reason = stop_sending_requested_ && stop_sending_reason_ != common::IoErr::None
                                                          ? stop_sending_reason_
                                                          : common::IoErr::Canceled;
        close_all_streams(stream_close_reason);
        clear_transport_callbacks();
        cancel_io_timers();
        if (transport_->valid()) {
            transport_->close();
        }
    }
    if (streams_.size() != 0) {
        return;
    }
    state_ = State::Closed;
    if (!io_pump_running_) {
        schedule_closed_completion();
    }
}

void Http2Connection::on_closed_completion(Http2Connection *connection) noexcept {
    FIBER_ASSERT(connection != nullptr);
    connection->close_completion_posted_ = false;
    connection->dispatch_closed_completion();
}

void Http2Connection::schedule_closed_completion() noexcept {
    if (close_completion_posted_ || close_completion_dispatched_) {
        return;
    }
    close_completion_posted_ = true;
    transport_->loop()
            .post_local<Http2Connection, &Http2Connection::close_completion_entry_,
                        &Http2Connection::on_closed_completion>(*this);
}

void Http2Connection::dispatch_closed_completion() noexcept {
    if (close_completion_dispatched_) {
        return;
    }
    close_completion_dispatched_ = true;
    std::coroutine_handle<> waiter = std::exchange(closed_waiter_, {});
    if (waiter) {
        FIBER_ASSERT(on_closed_ == nullptr);
        waiter.resume();
        return;
    }
    ClosedCallback callback = std::exchange(on_closed_, nullptr);
    void *callback_ctx = std::exchange(closed_ctx_, nullptr);
    if (callback) {
        RunResult result =
                terminal_error_ == common::IoErr::None ? RunResult{} : RunResult(std::unexpected(terminal_error_));
        callback(callback_ctx, *this, std::move(result));
    }
}

fiber::async::Task<void> Http2Connection::stop_and_wait_closed(common::IoErr reason) noexcept {
    if (state_ != State::Init && state_ != State::Closed) {
        enter_closing(reason, false);
    }
    if (state_ != State::Init) {
        (void) co_await wait_closed();
    }
}

void Http2Connection::shutdown(common::IoErr reason) noexcept { enter_closing(reason, false); }

void Http2Connection::graceful_shutdown() noexcept {
    common::IoErr err = start_draining();
    if (err != common::IoErr::None && err != common::IoErr::Canceled) {
        enter_closing(err);
    }
}

common::IoErr Http2Connection::on_frame_payload(const FrameHeader &, const mem::IoBuf &, std::size_t,
                                                std::size_t) noexcept {
    return common::IoErr::None;
}

common::IoErr Http2Connection::consume_incoming_frame_payload(const FrameHeader &fhr, const mem::IoBuf &buf,
                                                              std::size_t offset, std::size_t length) noexcept {
    if (inbound_stream_.header_block_open && fhr.type != Http2FrameType::Continuation) {
        return common::IoErr::Invalid;
    }
    if (!inbound_stream_.header_block_open && fhr.type == Http2FrameType::Continuation) {
        return common::IoErr::Invalid;
    }

    switch (fhr.type) {
        case Http2FrameType::Data:
            return handle_data_payload(fhr, buf, offset, length);
        case Http2FrameType::Headers:
            return handle_headers_payload(fhr, buf, offset, length);
        case Http2FrameType::Continuation:
            return handle_continuation_payload(fhr, buf, offset, length);
        case Http2FrameType::Settings:
            return handle_settings_payload(fhr, buf, offset, length);
        case Http2FrameType::Ping:
            return handle_ping_payload(fhr, buf, offset, length);
        case Http2FrameType::WindowUpdate:
            return handle_window_update_payload(fhr, buf, offset, length);
        case Http2FrameType::RstStream:
            return handle_rst_stream_payload(fhr, buf, offset, length);
        case Http2FrameType::Goaway:
            return handle_goaway_payload(fhr, buf, offset, length);
        case Http2FrameType::PriorityUpdate:
            // RFC 9218 PRIORITY_UPDATE is an optional extension frame. We do
            // not implement reprioritization yet, so ignore it like any other
            // unsupported extension frame.
            return common::IoErr::None;
        default:
            return on_frame_payload(fhr, buf, offset, length);
    }
}

common::IoErr Http2Connection::handle_data_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                   std::size_t length) noexcept {
    if (offset == 0) {
        if (fhr.stream_id == 0) {
            return common::IoErr::Invalid;
        }
        Http2Stream *stream = find_stream(fhr.stream_id);
        if (!stream) {
            return common::IoErr::Invalid;
        }

        std::uint8_t pad_length = 0;
        if ((fhr.flags & kFlagPadded) != 0) {
            pad_length = buf.readable_data()[0];
            if (fhr.length < static_cast<std::uint32_t>(1 + pad_length)) {
                return common::IoErr::Invalid;
            }
        }

        if (conn_recv_window_remaining_ < static_cast<std::int32_t>(fhr.length)) {
            return common::IoErr::Invalid;
        }
        conn_recv_window_remaining_ -= static_cast<std::int32_t>(fhr.length);

        if (stream->recv_window_remaining() < static_cast<std::int32_t>(fhr.length)) {
            handle_stream_error(fhr.stream_id, Http2ErrorCode::FlowControlError, common::IoErr::Invalid);
            return common::IoErr::None;
        }
        stream->consume_recv_window(fhr.length);

        inbound_stream_.lease = stream->lease();
        inbound_stream_.stream_id = fhr.stream_id;
        inbound_stream_.payload_begin = (fhr.flags & kFlagPadded) != 0 ? 1U : 0U;
        inbound_stream_.payload_end = fhr.length - pad_length;
        inbound_stream_.header_block_open = false;
        inbound_stream_.end_stream_pending = false;

        common::IoErr conn_err = maybe_replenish_connection_recv_window();
        if (conn_err != common::IoErr::None) {
            return conn_err;
        }
    }

    Http2Stream *stream = inbound_stream_.lease.get();
    if (!stream || inbound_stream_.stream_id != fhr.stream_id) {
        return common::IoErr::Invalid;
    }

    std::size_t logical_total = inbound_stream_.payload_end - inbound_stream_.payload_begin;
    std::size_t frame_start = offset;
    std::size_t frame_end = offset + length;
    std::size_t data_begin = inbound_stream_.payload_begin;
    std::size_t data_end = inbound_stream_.payload_end;
    std::size_t deliver_begin = std::max(frame_start, data_begin);
    std::size_t deliver_end = std::min(frame_end, data_end);
    std::size_t chunk_begin = deliver_begin > frame_start ? deliver_begin - frame_start : 0;
    std::size_t deliver_len = deliver_end > deliver_begin ? deliver_end - deliver_begin : 0;
    std::size_t data_pos = std::min(std::max(frame_start, data_begin), data_end);
    std::size_t data_offset = data_pos > data_begin ? data_pos - data_begin : 0;
    bool end_stream = ((fhr.flags & kFlagEndStream) != 0) && (frame_end >= fhr.length);

    if (deliver_len != 0 || end_stream) {
        mem::IoBuf payload = buf.retain_slice(chunk_begin, deliver_len);
        if (!payload && deliver_len != 0) {
            return common::IoErr::NoMem;
        }
        common::IoErr err = stream->on_data_payload_recv(std::move(payload), data_offset, logical_total, end_stream);
        if (err != common::IoErr::None) {
            handle_stream_error(fhr.stream_id, Http2ErrorCode::StreamClosed, err);
            clear_inbound_stream();
            return common::IoErr::None;
        }
        try_release_stream(*stream);
    }

    if (frame_end >= fhr.length) {
        clear_inbound_stream();
    }

    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_headers_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                      std::size_t length) noexcept {
    if (offset == 0) {
        if (fhr.stream_id == 0) {
            return common::IoErr::Invalid;
        }
        std::uint8_t pad_length = 0;
        if ((fhr.flags & kFlagPadded) != 0) {
            pad_length = buf.readable_data()[0];
        }
        std::size_t frame_prefix =
                ((fhr.flags & kFlagPadded) != 0 ? 1U : 0U) + ((fhr.flags & kFlagPriority) != 0 ? 5U : 0U);
        if (fhr.length < frame_prefix + pad_length) {
            return common::IoErr::Invalid;
        }

        Http2Stream *stream = find_stream(fhr.stream_id);
        if (!stream) {
            if (!is_idle_stream(fhr.stream_id)) {
                return common::IoErr::Invalid;
            }
            stream = create_peer_stream(fhr.stream_id);
            if (!stream) {
                return common::IoErr::Invalid;
            }
        } else {
            if (stream->remote_end_stream_) {
                return common::IoErr::Invalid;
            }
        }

        inbound_stream_.lease = stream->lease();
        inbound_stream_.stream_id = fhr.stream_id;
        inbound_stream_.payload_begin = frame_prefix;
        inbound_stream_.payload_end = fhr.length - pad_length;
        inbound_stream_.header_block_open = (fhr.flags & kFlagEndHeaders) == 0;
        inbound_stream_.end_stream_pending = (fhr.flags & kFlagEndStream) != 0;
    }

    Http2Stream *stream = inbound_stream_.lease.get();
    if (!stream || inbound_stream_.stream_id != fhr.stream_id) {
        return common::IoErr::Invalid;
    }

    std::size_t frame_start = offset;
    std::size_t frame_end = offset + length;
    std::size_t fragment_begin = inbound_stream_.payload_begin;
    std::size_t fragment_end = inbound_stream_.payload_end;
    std::size_t deliver_begin = std::max(frame_start, fragment_begin);
    std::size_t deliver_end = std::min(frame_end, fragment_end);
    std::size_t chunk_begin = deliver_begin > frame_start ? deliver_begin - frame_start : 0;
    std::size_t deliver_len = deliver_end > deliver_begin ? deliver_end - deliver_begin : 0;
    bool end_headers = ((fhr.flags & kFlagEndHeaders) != 0) && (frame_end >= fhr.length);
    bool end_stream = end_headers && inbound_stream_.end_stream_pending;

    if (deliver_len != 0 || end_headers || end_stream) {
        mem::IoBuf payload = buf.retain_slice(chunk_begin, deliver_len);
        if (!payload && deliver_len != 0) {
            return common::IoErr::NoMem;
        }
        common::IoErr err =
                stream->on_headers_payload_recv(deliver_len != 0 ? payload : buf, offset == 0, end_headers, end_stream);
        if (err != common::IoErr::None) {
            handle_stream_error(fhr.stream_id, Http2ErrorCode::ProtocolError, err);
            clear_inbound_stream();
            return common::IoErr::None;
        }
        try_release_stream(*stream);
    }

    if (frame_end >= fhr.length) {
        if (end_headers) {
            clear_inbound_stream();
        } else {
            inbound_stream_.payload_begin = 0;
            inbound_stream_.payload_end = 0;
        }
    }

    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_continuation_payload(const FrameHeader &fhr, const mem::IoBuf &buf,
                                                           std::size_t offset, std::size_t length) noexcept {
    if (offset == 0) {
        if (!inbound_stream_.header_block_open || fhr.stream_id != inbound_stream_.stream_id ||
            !inbound_stream_.lease) {
            return common::IoErr::Invalid;
        }
        inbound_stream_.payload_begin = 0;
        inbound_stream_.payload_end = fhr.length;
    }

    Http2Stream *stream = inbound_stream_.lease.get();
    if (!stream) {
        return common::IoErr::Invalid;
    }

    std::size_t frame_start = offset;
    std::size_t frame_end = offset + length;
    bool end_headers = ((fhr.flags & kFlagEndHeaders) != 0) && (frame_end >= fhr.length);
    bool end_stream = end_headers && inbound_stream_.end_stream_pending;
    if (length != 0 || end_headers || end_stream) {
        mem::IoBuf payload = buf.retain_slice(0, length);
        if (!payload && length != 0) {
            return common::IoErr::NoMem;
        }
        common::IoErr err =
                stream->on_headers_payload_recv(length != 0 ? payload : buf, false, end_headers, end_stream);
        if (err != common::IoErr::None) {
            handle_stream_error(fhr.stream_id, Http2ErrorCode::ProtocolError, err);
            clear_inbound_stream();
            return common::IoErr::None;
        }
        try_release_stream(*stream);
    }

    if (frame_end >= fhr.length) {
        if (end_headers) {
            clear_inbound_stream();
        } else {
            inbound_stream_.payload_begin = 0;
            inbound_stream_.payload_end = 0;
        }
    }

    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_settings_payload(const FrameHeader &fhr, const mem::IoBuf &buf,
                                                       std::size_t offset, std::size_t length) noexcept {
    if (offset == 0) {
        settings_scratch_used_ = 0;
        if (fhr.stream_id != 0) {
            return common::IoErr::Invalid;
        }
        if ((fhr.flags & kFlagSettingsAck) != 0) {
            if (fhr.length != 0) {
                return common::IoErr::Invalid;
            }
            return common::IoErr::None;
        }
        if ((fhr.length % kSettingsParameterSize) != 0) {
            return common::IoErr::Invalid;
        }
    }

    if ((fhr.flags & kFlagSettingsAck) != 0) {
        return common::IoErr::None;
    }

    const std::uint8_t *pos = buf.readable_data();
    std::size_t remaining = length;
    while (remaining != 0) {
        std::size_t take = std::min<std::size_t>(remaining, kSettingsParameterSize - settings_scratch_used_);
        std::memcpy(settings_scratch_.data() + settings_scratch_used_, pos, take);
        settings_scratch_used_ += take;
        pos += take;
        remaining -= take;

        if (settings_scratch_used_ == kSettingsParameterSize) {
            std::uint16_t id = parse_u16(settings_scratch_.data());
            std::uint32_t value = parse_u32(settings_scratch_.data() + 2);
            common::IoErr err = apply_settings_parameter(id, value);
            if (err != common::IoErr::None) {
                return err;
            }
            settings_scratch_used_ = 0;
        }
    }

    if (offset + length == fhr.length) {
        if (settings_scratch_used_ != 0) {
            return common::IoErr::Invalid;
        }
        peer_settings_received_ = true;
        return send_settings_ack();
    }

    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_ping_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                   std::size_t length) noexcept {
    if (offset == 0) {
        control_payload_used_ = 0;
        if (fhr.stream_id != 0 || fhr.length != kPingPayloadSize) {
            return common::IoErr::Invalid;
        }
    }

    if (length != 0) {
        std::memcpy(control_payload_scratch_.data() + control_payload_used_, buf.readable_data(), length);
        control_payload_used_ += length;
    }

    if (offset + length != fhr.length) {
        return common::IoErr::None;
    }

    if ((fhr.flags & kFlagAck) != 0) {
        if (keepalive_ping_outstanding_ && std::memcmp(control_payload_scratch_.data(), keepalive_ping_payload_.data(),
                                                       keepalive_ping_payload_.size()) == 0) {
            keepalive_ping_outstanding_ = false;
        }
        return common::IoErr::None;
    }

    return send_ping_ack(control_payload_scratch_.data());
}

common::IoErr Http2Connection::handle_window_update_payload(const FrameHeader &fhr, const mem::IoBuf &buf,
                                                            std::size_t offset, std::size_t length) noexcept {
    if (offset == 0) {
        control_payload_used_ = 0;
        if (fhr.length != kWindowUpdatePayloadSize) {
            return common::IoErr::Invalid;
        }
    }

    if (length != 0) {
        std::memcpy(control_payload_scratch_.data() + control_payload_used_, buf.readable_data(), length);
        control_payload_used_ += length;
    }

    if (offset + length != fhr.length) {
        return common::IoErr::None;
    }

    std::uint32_t increment = parse_u32(control_payload_scratch_.data()) & 0x7fffffffU;
    if (increment == 0) {
        if (fhr.stream_id == 0) {
            return common::IoErr::Invalid;
        }
        handle_stream_error(fhr.stream_id, Http2ErrorCode::ProtocolError, common::IoErr::Invalid);
        return common::IoErr::None;
    }

    if (fhr.stream_id == 0) {
        const std::int32_t current_window = conn_send_window_;
        std::int64_t next_window = static_cast<std::int64_t>(current_window) + static_cast<std::int64_t>(increment);
        if (next_window > kMaxFlowControlWindow) {
            return common::IoErr::Invalid;
        }
        update_connection_send_window(static_cast<std::int32_t>(increment));
        return common::IoErr::None;
    }

    Http2Stream *stream = streams_.find(fhr.stream_id);
    if (!stream) {
        return is_idle_stream(fhr.stream_id) ? common::IoErr::Invalid : common::IoErr::None;
    }

    std::int64_t next_window = static_cast<std::int64_t>(stream->send_window_) + static_cast<std::int64_t>(increment);
    if (next_window > kMaxFlowControlWindow) {
        handle_stream_error(fhr.stream_id, Http2ErrorCode::FlowControlError, common::IoErr::Invalid);
        return common::IoErr::None;
    }

    stream->update_send_window(static_cast<std::int32_t>(increment));
    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_rst_stream_payload(const FrameHeader &fhr, const mem::IoBuf &buf,
                                                         std::size_t offset, std::size_t length) noexcept {
    if (offset == 0) {
        control_payload_used_ = 0;
        if (fhr.stream_id == 0 || fhr.length != kRstStreamPayloadSize) {
            return common::IoErr::Invalid;
        }
    }

    if (length != 0) {
        std::memcpy(control_payload_scratch_.data() + control_payload_used_, buf.readable_data(), length);
        control_payload_used_ += length;
    }

    if (offset + length != fhr.length) {
        return common::IoErr::None;
    }

    Http2Stream *stream = streams_.find(fhr.stream_id);
    if (!stream) {
        return is_idle_stream(fhr.stream_id) ? common::IoErr::Invalid : common::IoErr::None;
    }

    stream->on_rst_recv(static_cast<Http2ErrorCode>(parse_u32(control_payload_scratch_.data())));
    try_release_stream(*stream);
    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_goaway_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                     std::size_t length) noexcept {
    if (offset == 0) {
        control_payload_used_ = 0;
        if (fhr.stream_id != 0 || fhr.length < kGoawayMinimumPayloadSize) {
            return common::IoErr::Invalid;
        }
    }

    std::size_t take = std::min<std::size_t>(length, control_payload_scratch_.size() - control_payload_used_);
    if (take != 0) {
        std::memcpy(control_payload_scratch_.data() + control_payload_used_, buf.readable_data(), take);
        control_payload_used_ += take;
    }

    if (offset + length != fhr.length) {
        return common::IoErr::None;
    }

    std::uint32_t last_stream_id = parse_stream_id(control_payload_scratch_.data());
    Http2ErrorCode error_code = static_cast<Http2ErrorCode>(parse_u32(control_payload_scratch_.data() + 4));
    handle_peer_goaway(last_stream_id, error_code);
    return common::IoErr::None;
}

common::IoErr Http2Connection::apply_settings_parameter(std::uint16_t id, std::uint32_t value) noexcept {
    switch (id) {
        case kSettingsHeaderTableSize:
            // Every outbound header block starts by selecting a zero-sized
            // dynamic table, so the peer's upper bound cannot restrict it.
            return common::IoErr::None;
        case kSettingsEnablePush:
            if (value > 1) {
                return common::IoErr::Invalid;
            }
            peer_enable_push_ = value != 0;
            return common::IoErr::None;
        case kSettingsMaxConcurrentStreams:
            peer_advertised_max_concurrent_streams_ = value;
            return common::IoErr::None;
        case kSettingsInitialWindowSize:
            return apply_peer_initial_stream_window(value);
        case kSettingsMaxFrameSize:
            if (value < kDefaultMaxFrameSize || value > kMaxFrameSizeLimit) {
                return common::IoErr::Invalid;
            }
            peer_max_outbound_frame_size_ = value;
            return common::IoErr::None;
        case kSettingsMaxHeaderListSize:
            peer_max_header_list_size_ = value;
            return common::IoErr::None;
        case kSettingsEnableConnectProtocol:
            if (value > 1) {
                return common::IoErr::Invalid;
            }
            peer_enable_connect_protocol_ = value != 0;
            return common::IoErr::None;
        default:
            return common::IoErr::None;
    }
}

common::IoErr Http2Connection::apply_peer_initial_stream_window(std::uint32_t value) noexcept {
    if (value > static_cast<std::uint32_t>(kMaxFlowControlWindow)) {
        return common::IoErr::Invalid;
    }

    std::int64_t delta = static_cast<std::int64_t>(value) - static_cast<std::int64_t>(peer_initial_stream_send_window_);
    if (delta != 0) {
        common::IoErr err = common::IoErr::None;
        streams_.for_each([&](Http2Stream &stream) {
            if (err != common::IoErr::None) {
                return;
            }
            std::int64_t next_window = static_cast<std::int64_t>(stream.send_window()) + delta;
            if (next_window > kMaxFlowControlWindow || next_window < -kMaxFlowControlWindow - 1) {
                err = common::IoErr::Invalid;
                return;
            }
            stream.update_send_window(static_cast<std::int32_t>(delta));
        });
        if (err != common::IoErr::None) {
            // A SETTINGS flow-control error is connection-fatal, so streams already
            // updated during this pass are torn down and do not need rollback.
            return err;
        }
    }

    peer_initial_stream_send_window_ = static_cast<std::int32_t>(value);
    return common::IoErr::None;
}

common::IoErr Http2Connection::send_initial_flight() noexcept {
    const std::size_t settings_count = options_.enable_connect_protocol ? 4 : 3;
    const std::size_t settings_payload_size = settings_count * kSettingsParameterSize;
    bool send_client_preface = options_.role == ConnectionRole::Client;
    bool send_conn_window_update =
            options_.initial_connection_recv_window > static_cast<std::uint32_t>(kInitialFlowControlWindow);
    std::size_t total_size = kFrameHeaderSize + settings_payload_size;
    if (send_client_preface) {
        total_size += kClientPreface.size();
    }
    if (send_conn_window_update) {
        total_size += kFrameHeaderSize + kWindowUpdatePayloadSize;
    }
    return alloc_and_enqueue_control(total_size, [&](std::uint8_t *dst) noexcept {
        std::uint8_t *out = dst;
        if (send_client_preface) {
            std::memcpy(out, kClientPreface.data(), kClientPreface.size());
            out += kClientPreface.size();
        }

        encode_http2_frame_header(out, static_cast<std::uint32_t>(settings_payload_size), Http2FrameType::Settings, 0,
                                  0);
        out += kFrameHeaderSize;
        out = append_u16(out, kSettingsMaxConcurrentStreams);
        out = append_u32(out, options_.local_max_concurrent_streams);
        out = append_u16(out, kSettingsInitialWindowSize);
        out = append_u32(out, configured_initial_stream_recv_window());
        out = append_u16(out, kSettingsMaxFrameSize);
        out = append_u32(out, options_.max_frame_size);
        if (options_.enable_connect_protocol) {
            out = append_u16(out, kSettingsEnableConnectProtocol);
            out = append_u32(out, 1);
        }

        if (send_conn_window_update) {
            std::uint32_t increment =
                    options_.initial_connection_recv_window - static_cast<std::uint32_t>(kInitialFlowControlWindow);
            encode_http2_frame_header(out, kWindowUpdatePayloadSize, Http2FrameType::WindowUpdate, 0, 0);
            out += kFrameHeaderSize;
            out = append_u32(out, increment & 0x7fffffffU);
        }
    });
}

common::IoErr Http2Connection::send_settings_ack() noexcept {
    return alloc_and_enqueue_control(kFrameHeaderSize, [&](std::uint8_t *dst) noexcept {
        encode_http2_frame_header(dst, 0, Http2FrameType::Settings, kFlagSettingsAck, 0);
    });
}

common::IoErr Http2Connection::send_ping_ack(const std::uint8_t *opaque_data) noexcept {
    return alloc_and_enqueue_control(kFrameHeaderSize + kPingPayloadSize, [&](std::uint8_t *dst) noexcept {
        encode_http2_frame_header(dst, kPingPayloadSize, Http2FrameType::Ping, kFlagAck, 0);
        std::memcpy(dst + kFrameHeaderSize, opaque_data, kPingPayloadSize);
    });
}

common::IoErr Http2Connection::send_window_update(std::uint32_t stream_id, std::uint32_t increment) noexcept {
    if (increment == 0 || increment > static_cast<std::uint32_t>(kMaxFlowControlWindow)) {
        return common::IoErr::Invalid;
    }

    return alloc_and_enqueue_control(kFrameHeaderSize + kWindowUpdatePayloadSize, [&](std::uint8_t *dst) noexcept {
        encode_http2_frame_header(dst, kWindowUpdatePayloadSize, Http2FrameType::WindowUpdate, 0, stream_id);
        append_u32(dst + kFrameHeaderSize, increment);
    });
}

common::IoErr Http2Connection::send_rst_stream(std::uint32_t stream_id, Http2ErrorCode error_code) noexcept {
    return alloc_and_enqueue_control(kFrameHeaderSize + kRstStreamPayloadSize, [&](std::uint8_t *dst) noexcept {
        encode_http2_frame_header(dst, kRstStreamPayloadSize, Http2FrameType::RstStream, 0, stream_id);
        append_u32(dst + kFrameHeaderSize, static_cast<std::uint32_t>(error_code));
    });
}

common::IoErr Http2Connection::send_goaway(std::uint32_t last_stream_id, Http2ErrorCode error_code) noexcept {
    return alloc_and_enqueue_control(kFrameHeaderSize + kGoawayMinimumPayloadSize, [&](std::uint8_t *dst) noexcept {
        encode_http2_frame_header(dst, kGoawayMinimumPayloadSize, Http2FrameType::Goaway, 0, 0);
        std::uint8_t *out = append_u32(dst + kFrameHeaderSize, last_stream_id & 0x7fffffffU);
        append_u32(out, static_cast<std::uint32_t>(error_code));
    });
}

common::IoErr Http2Connection::maybe_replenish_connection_recv_window() noexcept {
    if (conn_recv_window_remaining_ > static_cast<std::int32_t>(options_.connection_recv_window_low_watermark)) {
        return common::IoErr::None;
    }

    if (conn_recv_window_remaining_ >= static_cast<std::int32_t>(conn_recv_window_target_)) {
        return common::IoErr::None;
    }

    std::uint32_t increment = conn_recv_window_target_ - static_cast<std::uint32_t>(conn_recv_window_remaining_);
    common::IoErr err = send_window_update(0, increment);
    if (err != common::IoErr::None) {
        return err;
    }
    conn_recv_window_remaining_ = static_cast<std::int32_t>(conn_recv_window_target_);
    return common::IoErr::None;
}

std::uint32_t Http2Connection::configured_initial_stream_recv_window() const noexcept {
    if (options_.initial_stream_recv_window != 0) {
        return options_.initial_stream_recv_window;
    }
    return static_cast<std::uint32_t>(options_.initial_stream_send_window);
}

void Http2Connection::handle_stream_error(std::uint32_t stream_id, Http2ErrorCode error_code,
                                          common::IoErr pending_result) noexcept {
    (void) send_rst_stream(stream_id, error_code);

    Http2Stream *stream = find_stream(stream_id);
    if (!stream) {
        return;
    }

    stream->close(pending_result);
    try_release_stream(*stream);
}

Http2Stream *Http2Connection::find_stream(std::uint32_t stream_id) noexcept { return streams_.find(stream_id); }

const Http2Stream *Http2Connection::find_stream(std::uint32_t stream_id) const noexcept {
    return streams_.find(stream_id);
}

Http2Stream *Http2Connection::create_peer_stream(std::uint32_t stream_id) noexcept {
    if (!can_accept_peer_stream(stream_id) || find_stream(stream_id)) {
        return nullptr;
    }

    Http2Stream::Lease stream = alloc_peer_stream(stream_id);
    if (!stream) {
        return nullptr;
    }
    Http2Stream *stream_ptr = stream.get();
    stream_ptr->attach_to_connection(*this, stream_id);
    stream_ptr->active_ = true;
    stream_ptr->send_window_ = peer_initial_stream_send_window_;
    stream_ptr->recv_window_target_ = configured_initial_stream_recv_window();
    stream_ptr->recv_window_low_watermark_ = options_.stream_recv_window_low_watermark;
    stream_ptr->recv_window_remaining_ = static_cast<std::int32_t>(stream_ptr->recv_window_target_);
    if (!streams_.insert(std::move(stream))) {
        stream_ptr->attached_to_connection_ = false;
        stream_ptr->conn_ = nullptr;
        stream_ptr->stream_id_ = 0;
        stream_ptr->active_ = false;
        stream_ptr->close_reason_ = common::IoErr::NoMem;
        return nullptr;
    }
    owned_stream_list_.push_back(*stream_ptr);

    last_peer_stream_id_ = stream_id;
    ++peer_active_stream_count_;
    return stream_ptr;
}

common::IoResult<Http2Stream::Lease> Http2Connection::attach_local_stream(Http2Stream &stream) noexcept {
    if (!can_attach_local_stream()) {
        return std::unexpected(common::IoErr::Busy);
    }
    if (stream.attached_to_connection_ || stream.conn_ != nullptr || stream.stream_id_ != 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint32_t stream_id = next_local_stream_id_;
    if (find_stream(stream_id) != nullptr) {
        return std::unexpected(common::IoErr::Already);
    }

    stream.attach_to_connection(*this, stream_id);
    stream.active_ = true;
    stream.send_window_ = peer_initial_stream_send_window_;
    stream.recv_window_target_ = configured_initial_stream_recv_window();
    stream.recv_window_low_watermark_ = options_.stream_recv_window_low_watermark;
    stream.recv_window_remaining_ = static_cast<std::int32_t>(stream.recv_window_target_);

    Http2Stream::Lease lease = Http2Stream::Lease::adopt(&stream);
    if (!streams_.insert(std::move(lease))) {
        (void) lease.release_raw();
        stream.attached_to_connection_ = false;
        stream.conn_ = nullptr;
        stream.stream_id_ = 0;
        stream.active_ = false;
        return std::unexpected(common::IoErr::NoMem);
    }
    owned_stream_list_.push_back(stream);

    last_local_stream_id_ = stream_id;
    next_local_stream_id_ += 2U;
    ++local_active_stream_count_;
    return stream.lease();
}

void Http2Connection::detach_stream(Http2Stream &stream) noexcept {
    if (!stream.attached_to_connection_) {
        return;
    }

    Http2Stream::Lease held = streams_.erase(stream.stream_id_);
    if (is_peer_stream_id(stream.stream_id_) && peer_active_stream_count_ != 0) {
        --peer_active_stream_count_;
    }
    if (is_local_stream_id(stream.stream_id_) && local_active_stream_count_ != 0) {
        --local_active_stream_count_;
    }
    owned_stream_list_.erase(stream);
    stream.attached_to_connection_ = false;
    stream.conn_ = nullptr;
    stream.active_ = false;
    if (state_ == State::Closing) {
        finish_connection();
        return;
    }
    maybe_enter_closing_from_draining();
}

void Http2Connection::try_release_stream(Http2Stream &stream) noexcept {
    if (!stream.ready_for_connection_release()) {
        return;
    }
    if (!stream.attached_to_connection_) {
        return;
    }
    if (stream.outbound_hook_.state_ != Http2OutboundHook::State::Idle ||
        stream.outbound_wait_state_ != Http2Stream::OutboundWaitState::None ||
        stream.outbound_kind_ != Http2OutboundKind::None) {
        return;
    }
    if (inbound_stream_.header_block_open && inbound_stream_.lease.get() == &stream) {
        return;
    }
    detach_stream(stream);
}

bool Http2Connection::can_accept_peer_stream(std::uint32_t stream_id) const noexcept {
    return state_ == State::Running && stream_id != 0 && is_peer_stream_id(stream_id) &&
           is_next_peer_stream_id(stream_id) && peer_active_stream_count_ < options_.local_max_concurrent_streams;
}

bool Http2Connection::can_attach_local_stream() const noexcept {
    const bool local_session_ready =
            state_ == State::Running || (options_.role == ConnectionRole::Client && state_ == State::Start);
    return local_session_ready && !peer_goaway_received_ && is_local_stream_id(next_local_stream_id_) &&
           local_active_stream_count_ < peer_advertised_max_concurrent_streams_;
}

bool Http2Connection::is_next_peer_stream_id(std::uint32_t stream_id) const noexcept {
    return is_peer_stream_id(stream_id) && stream_id > last_peer_stream_id_;
}

void Http2Connection::handle_peer_goaway(std::uint32_t last_stream_id, Http2ErrorCode error_code) noexcept {
    peer_goaway_received_ = true;
    peer_last_stream_id_ = last_stream_id;
    peer_goaway_error_code_ = error_code;
    if (state_ == State::Running) {
        state_ = State::Draining;
    }
    close_streams_after_goaway(last_stream_id);
    if (!local_goaway_sent_ && state_ != State::Closing && state_ != State::Closed) {
        local_goaway_last_stream_id_ = last_peer_stream_id_;
        common::IoErr err = send_goaway(local_goaway_last_stream_id_, Http2ErrorCode::NoError);
        if (err != common::IoErr::None) {
            enter_closing(err);
            return;
        }
        local_goaway_sent_ = true;
    }
    maybe_enter_closing_from_draining();
}

void Http2Connection::close_streams_after_goaway(std::uint32_t last_stream_id) noexcept {
    for (Http2Stream *stream = owned_stream_list_.front(); stream != nullptr;) {
        Http2Stream *next = owned_stream_list_.next_of(*stream);
        if (is_local_stream_id(stream->stream_id_) && stream->stream_id_ > last_stream_id) {
            Http2Stream::Lease held = stream->lease();
            stream->close(common::IoErr::Canceled);
            try_release_stream(*stream);
        }
        stream = next;
    }
}

bool Http2Connection::is_idle_stream(std::uint32_t stream_id) const noexcept {
    if (stream_id == 0 || streams_.find(stream_id)) {
        return false;
    }
    if (is_local_stream_id(stream_id)) {
        return stream_id > last_local_stream_id_;
    }
    if (is_peer_stream_id(stream_id)) {
        return stream_id > last_peer_stream_id_;
    }
    return false;
}

bool Http2Connection::is_local_stream_id(std::uint32_t stream_id) const noexcept {
    if (stream_id == 0) {
        return false;
    }
    bool odd = (stream_id & 1U) != 0;
    if (options_.role == ConnectionRole::Client) {
        return odd;
    }
    return !odd;
}

bool Http2Connection::is_peer_stream_id(std::uint32_t stream_id) const noexcept {
    if (stream_id == 0) {
        return false;
    }
    return !is_local_stream_id(stream_id);
}

std::chrono::milliseconds Http2Connection::current_read_timeout() const noexcept {
    if (keepalive_ping_outstanding_) {
        return options_.read_timeout;
    }
    if (state_ == State::Running && options_.keepalive_ping_interval.count() > 0) {
        return options_.keepalive_ping_interval;
    }
    return options_.read_timeout;
}

void Http2Connection::update_connection_send_window(std::int32_t delta) noexcept {
    conn_send_window_ += delta;
    if (delta > 0) {
        wake_connection_window_waiters();
    }
}

common::IoErr Http2Connection::handle_read_timeout() noexcept {
    if (stop_sending_requested_ || state_ != State::Running || options_.keepalive_ping_interval.count() <= 0) {
        return common::IoErr::TimedOut;
    }
    if (keepalive_ping_outstanding_) {
        return common::IoErr::TimedOut;
    }
    return send_keepalive_ping();
}

common::IoErr Http2Connection::send_keepalive_ping() noexcept {
    ++keepalive_ping_sequence_;
    append_u64(keepalive_ping_payload_.data(), keepalive_ping_sequence_);
    common::IoErr err = alloc_and_enqueue_control(
            kFrameHeaderSize + keepalive_ping_payload_.size(), [&](std::uint8_t *dst) noexcept {
                encode_http2_frame_header(dst, static_cast<std::uint32_t>(keepalive_ping_payload_.size()),
                                          Http2FrameType::Ping, 0, 0);
                std::memcpy(dst + kFrameHeaderSize, keepalive_ping_payload_.data(), keepalive_ping_payload_.size());
            });
    if (err == common::IoErr::None) {
        keepalive_ping_outstanding_ = true;
        if (transport_) {
            inbound_io_.ping_sent_at = transport_->loop().now();
        }
    }
    return err;
}

common::IoErr Http2Connection::start_client_session() noexcept {
    common::IoErr err = send_initial_flight();
    if (err != common::IoErr::None) {
        stop_sending_requested_ = true;
        stop_sending_reason_ = err;
        abort_outbound(err);
        state_ = State::Closing;
        return err;
    }
    state_ = State::Running;
    return common::IoErr::None;
}

common::IoErr Http2Connection::start_server_session() noexcept {
    state_ = State::Start;
    return common::IoErr::None;
}

void Http2Connection::stop_sending(common::IoErr reason) noexcept { enter_closing(reason); }

common::IoErr Http2Connection::start_draining() noexcept {
    if (stop_sending_requested_) {
        return stop_sending_reason_;
    }
    if (state_ == State::Closed || state_ == State::Closing) {
        return common::IoErr::Canceled;
    }
    if (state_ == State::Init || state_ == State::Start) {
        return common::IoErr::Invalid;
    }
    if (state_ == State::Draining) {
        return common::IoErr::None;
    }

    local_goaway_last_stream_id_ = last_peer_stream_id_;
    common::IoErr err = send_goaway(local_goaway_last_stream_id_, Http2ErrorCode::NoError);
    if (err != common::IoErr::None) {
        enter_closing(err);
        return err;
    }

    local_goaway_sent_ = true;
    state_ = State::Draining;
    maybe_enter_closing_from_draining();
    return common::IoErr::None;
}

void Http2Connection::enter_closing(common::IoErr reason, bool report_error) noexcept {
    if (state_ == State::Closed) {
        return;
    }
    if (state_ == State::Init) {
        stop_sending_requested_ = true;
        stop_sending_reason_ = reason;
        if (report_error && reason != common::IoErr::None) {
            terminal_error_ = reason;
        }
        abort_outbound(reason);
        close_finished_ = true;
        close_completion_dispatched_ = true;
        state_ = State::Closed;
        return;
    }
    state_ = State::Closing;
    stop_sending_requested_ = true;
    stop_sending_reason_ = reason;
    if (report_error && reason != common::IoErr::None) {
        terminal_error_ = reason;
    }
    close_flush_outbound_ = false;
    clear_inbound_stream();
    inbound_io_.wait_event = event::IoEvent::None;
    inbound_io_.operation_pending = false;
    outbound_ready_hint_ = false;
    abort_outbound(reason);
    outbound_wait_event_ = event::IoEvent::None;
    finish_connection();
}

std::size_t Http2Connection::configured_max_active_streams() const noexcept {
    return static_cast<std::size_t>(options_.local_max_concurrent_streams) +
           std::max(static_cast<std::size_t>(options_.max_peer_concurrent_streams),
                    static_cast<std::size_t>(options_.max_local_push_streams));
}

void Http2Connection::bind_outbound_chain(mem::IoBufChain &chain) noexcept {
    event::EventLoop *owner_loop = transport_ ? &transport_->loop() : event::EventLoop::current_or_null();
    FIBER_ASSERT(owner_loop != nullptr);
    if (!chain.bound()) {
        chain.bind_node_pool(owner_loop->io_buf_node_pool());
    } else {
        FIBER_ASSERT(&chain.node_pool() == &owner_loop->io_buf_node_pool());
    }
}

void Http2Connection::enqueue_outbound_hook(Http2OutboundHook &hook, bool priority) noexcept {
    FIBER_ASSERT(hook.state_ == Http2OutboundHook::State::Idle);
    FIBER_ASSERT(hook.encoded_.readable_bytes() != 0);
    if (priority) {
        outbound_queue_.push_front(hook);
    } else {
        outbound_queue_.push_back(hook);
    }
    hook.state_ = Http2OutboundHook::State::Queued;
}

void Http2Connection::enqueue_connection_window_wait(Http2Stream &stream) noexcept {
    FIBER_ASSERT(stream.outbound_wait_state_ == Http2Stream::OutboundWaitState::None);
    connection_window_waiters_.push_back(stream);
    stream.outbound_wait_state_ = Http2Stream::OutboundWaitState::ConnectionWindow;
}

void Http2Connection::remove_connection_window_wait(Http2Stream &stream) noexcept {
    if (stream.outbound_wait_state_ != Http2Stream::OutboundWaitState::ConnectionWindow) {
        return;
    }
    connection_window_waiters_.erase(stream);
    stream.outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
}

common::IoErr Http2Connection::try_encode_stream_outbound(Http2Stream &stream) noexcept {
    FIBER_ASSERT(stream.conn_ == this);
    FIBER_ASSERT(stream.outbound_operation_ != nullptr);
    FIBER_ASSERT(stream.outbound_kind_ != Http2OutboundKind::None);
    FIBER_ASSERT(stream.outbound_hook_.state_ == Http2OutboundHook::State::Idle);
    FIBER_ASSERT(stream.outbound_wait_state_ == Http2Stream::OutboundWaitState::None);

    const std::size_t pending_flow_controlled = stream.pending_flow_controlled_bytes();
    std::uint32_t payload_budget = 0;
    if (stream.outbound_kind_ == Http2OutboundKind::Data && pending_flow_controlled != 0) {
        if (stream.send_window_ <= 0) {
            stream.outbound_wait_state_ = Http2Stream::OutboundWaitState::StreamWindow;
            return common::IoErr::None;
        }
        if (conn_send_window_ <= 0) {
            enqueue_connection_window_wait(stream);
            return common::IoErr::None;
        }

        payload_budget = static_cast<std::uint32_t>(
                std::min<std::size_t>({pending_flow_controlled, static_cast<std::size_t>(stream.send_window_),
                                       static_cast<std::size_t>(conn_send_window_),
                                       static_cast<std::size_t>(peer_max_outbound_frame_size_)}));
        FIBER_ASSERT(payload_budget != 0);
    }

    Http2OutboundEncodeRequest request;
    request.max_frame_size = peer_max_outbound_frame_size_;
    request.payload_budget = payload_budget;
    Http2OutboundEncodeTarget target(transport_->loop().io_buf_node_pool());
    Http2OutboundEncodeResult result;
    common::IoErr err = stream.encode_outbound_batch(request, target, result);
    if (err != common::IoErr::None) {
        return err;
    }
    if (target.empty() || result.flow_controlled_bytes > payload_budget ||
        (stream.outbound_kind_ == Http2OutboundKind::Headers && result.flow_controlled_bytes != 0)) {
        return common::IoErr::Invalid;
    }

    Http2OutboundHook &hook = stream.outbound_hook_;
    FIBER_ASSERT(hook.encoded_.empty());
    hook.encoded_ = target.take_chain();
    hook.ctx_ = &stream;
    hook.send_done_cb_ = &Http2Stream::on_outbound_hook_send_done;
    hook.window_consumed_ = result.flow_controlled_bytes;
    hook.completion_result_ = common::IoErr::None;
    hook.operation_final_batch_ = result.operation_final_batch;
    conn_send_window_ -= static_cast<std::int32_t>(result.flow_controlled_bytes);
    stream.send_window_ -= static_cast<std::int32_t>(result.flow_controlled_bytes);
    enqueue_outbound_hook(hook, false);
    schedule_io_pump();
    return common::IoErr::None;
}

common::IoErr Http2Connection::request_stream_send(Http2Stream &stream, Http2OutboundKind kind) noexcept {
    if (state_ == State::Init || state_ == State::Closed || !transport_ || !transport_->valid()) {
        return common::IoErr::Invalid;
    }
    if (stop_sending_requested_) {
        return stop_sending_reason_;
    }
    if (kind == Http2OutboundKind::None || stream.conn_ != this || stream.outbound_operation_ == nullptr) {
        return common::IoErr::Invalid;
    }
    if (stream.outbound_kind_ != Http2OutboundKind::None ||
        stream.outbound_hook_.state_ != Http2OutboundHook::State::Idle ||
        stream.outbound_wait_state_ != Http2Stream::OutboundWaitState::None) {
        return common::IoErr::Already;
    }

    stream.outbound_kind_ = kind;
    common::IoErr err = try_encode_stream_outbound(stream);
    if (err != common::IoErr::None) {
        stream.outbound_kind_ = Http2OutboundKind::None;
    }
    return err;
}

bool Http2Connection::cancel_queued_stream_send(Http2Stream &stream) noexcept {
    if (stream.outbound_wait_state_ == Http2Stream::OutboundWaitState::ConnectionWindow) {
        remove_connection_window_wait(stream);
        stream.outbound_kind_ = Http2OutboundKind::None;
        return true;
    }
    if (stream.outbound_wait_state_ == Http2Stream::OutboundWaitState::StreamWindow) {
        stream.outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
        stream.outbound_kind_ = Http2OutboundKind::None;
        return true;
    }
    if (stream.outbound_hook_.state_ != Http2OutboundHook::State::Queued) {
        return false;
    }

    stream.outbound_kind_ = Http2OutboundKind::None;
    abandon_queued_stream_hook(stream, true);
    return true;
}

void Http2Connection::abandon_queued_stream_hook(Http2Stream &stream, bool restore_stream_window) noexcept {
    Http2OutboundHook &hook = stream.outbound_hook_;
    FIBER_ASSERT(hook.state_ == Http2OutboundHook::State::Queued);
    outbound_queue_.erase(hook);
    conn_send_window_ += static_cast<std::int32_t>(hook.window_consumed_);
    if (restore_stream_window) {
        stream.send_window_ += static_cast<std::int32_t>(hook.window_consumed_);
    }
    hook.encoded_.clear();
    hook.window_consumed_ = 0;
    hook.completion_result_ = common::IoErr::None;
    hook.operation_final_batch_ = false;
    hook.state_ = Http2OutboundHook::State::Idle;
    wake_connection_window_waiters();
}

void Http2Connection::cancel_stream_send(Http2Stream &stream, common::IoErr reason) noexcept {
    if (stream.outbound_wait_state_ == Http2Stream::OutboundWaitState::ConnectionWindow) {
        remove_connection_window_wait(stream);
    } else if (stream.outbound_wait_state_ == Http2Stream::OutboundWaitState::StreamWindow) {
        stream.outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
    }

    stream.outbound_kind_ = Http2OutboundKind::None;
    if (stream.outbound_hook_.state_ == Http2OutboundHook::State::Queued) {
        // The DATA never reached the transport, so only the connection-level
        // reservation is reusable. The closing stream's window is discarded.
        abandon_queued_stream_hook(stream, false);
    }
    if (stream.outbound_hook_.state_ == Http2OutboundHook::State::InFlight) {
        stream.outbound_hook_.completion_result_ = reason;
    } else if (stream.outbound_operation_) {
        stream.outbound_operation_->on_outbound_abort(reason);
    }
}

void Http2Connection::on_stream_send_window_update(Http2Stream &stream) noexcept {
    if (stream.outbound_wait_state_ != Http2Stream::OutboundWaitState::StreamWindow || stream.send_window_ <= 0 ||
        stream.close_reason_ != common::IoErr::None) {
        return;
    }
    stream.outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
    common::IoErr err = try_encode_stream_outbound(stream);
    if (err != common::IoErr::None) {
        enter_closing(err);
    }
}

void Http2Connection::wake_connection_window_waiters() noexcept {
    while (conn_send_window_ > 0) {
        Http2Stream *stream = connection_window_waiters_.front();
        if (!stream) {
            return;
        }
        connection_window_waiters_.erase(*stream);
        stream->outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
        if (stream->close_reason_ != common::IoErr::None) {
            stream->outbound_kind_ = Http2OutboundKind::None;
            continue;
        }
        common::IoErr err = try_encode_stream_outbound(*stream);
        if (err != common::IoErr::None) {
            enter_closing(err);
            return;
        }
    }
}

void Http2Connection::build_outbound_batch(std::size_t operation_budget, std::size_t byte_budget) noexcept {
    FIBER_ASSERT(inflight_outbound_chain_.empty());
    FIBER_ASSERT(inflight_outbound_hooks_.empty());
    operation_budget = std::max<std::size_t>(operation_budget, 1);
    byte_budget = std::max<std::size_t>(byte_budget, 1);

    std::size_t selected = 0;
    while (selected < operation_budget) {
        Http2OutboundHook *hook = outbound_queue_.front();
        if (!hook) {
            break;
        }
        const std::size_t hook_bytes = hook->encoded_.readable_bytes();
        FIBER_ASSERT(hook_bytes != 0);
        if (selected != 0 && inflight_outbound_chain_.readable_bytes() >= byte_budget) {
            break;
        }

        outbound_queue_.erase(*hook);
        const bool transferred = hook->encoded_.take_prefix(hook_bytes, inflight_outbound_chain_);
        FIBER_ASSERT(transferred);
        hook->inflight_wire_bytes_ = hook_bytes;
        hook->state_ = Http2OutboundHook::State::InFlight;
        inflight_outbound_hooks_.push_back(*hook);
        ++selected;
    }
}

void Http2Connection::finish_written_outbound_hooks(std::size_t bytes_written) noexcept {
    std::size_t remaining = bytes_written;
    while (remaining != 0) {
        Http2OutboundHook *hook = inflight_outbound_hooks_.front();
        FIBER_ASSERT(hook != nullptr);
        const std::size_t consumed = std::min(remaining, hook->inflight_wire_bytes_);
        hook->inflight_wire_bytes_ -= consumed;
        remaining -= consumed;
        if (hook->inflight_wire_bytes_ != 0) {
            break;
        }

        inflight_outbound_hooks_.erase(*hook);
        hook->state_ = Http2OutboundHook::State::Idle;
        if (hook == &control_hook_ && !hook->encoded_.empty()) {
            enqueue_outbound_hook(*hook, true);
        }
        common::IoErr completion_result = std::exchange(hook->completion_result_, common::IoErr::None);
        if (hook->send_done_cb_) {
            hook->send_done_cb_(*hook, completion_result);
        }
    }
    FIBER_ASSERT(remaining == 0);
}

common::IoResult<Http2Connection::OutboundPumpResult> Http2Connection::pump_outbound(std::size_t operation_budget,
                                                                                     std::size_t byte_budget) noexcept {
    OutboundPumpResult result;
    if (outbound_stopped_) {
        return result;
    }
    if (inflight_outbound_chain_.empty()) {
        build_outbound_batch(operation_budget, byte_budget);
    }
    if (inflight_outbound_chain_.empty()) {
        if (outbound_closed_ && outbound_idle()) {
            outbound_stopped_ = true;
        }
        return result;
    }
    if (!transport_ || !transport_->valid()) {
        abort_outbound(common::IoErr::Invalid);
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t written = 0;
    event::IoEvent wait_event = event::IoEvent::None;
    common::IoErr err = transport_->poll_writev(inflight_outbound_chain_, written, wait_event);
    if (err == common::IoErr::WouldBlock) {
        if (wait_event != event::IoEvent::Read && wait_event != event::IoEvent::Write) {
            abort_outbound(common::IoErr::Invalid);
            return std::unexpected(common::IoErr::Invalid);
        }
        result.wait_event = wait_event;
        return result;
    }
    if (err != common::IoErr::None) {
        abort_outbound(err);
        return std::unexpected(err);
    }
    if (written == 0) {
        abort_outbound(common::IoErr::ConnReset);
        return std::unexpected(common::IoErr::ConnReset);
    }

    result.bytes_written = written;
    finish_written_outbound_hooks(written);
    result.needs_reschedule = !inflight_outbound_chain_.empty() || !outbound_queue_.empty();
    if (outbound_closed_ && outbound_idle()) {
        outbound_stopped_ = true;
        result.needs_reschedule = false;
    }
    return result;
}

bool Http2Connection::outbound_idle() const noexcept {
    return outbound_queue_.empty() && inflight_outbound_hooks_.empty() && inflight_outbound_chain_.empty() &&
           connection_window_waiters_.empty();
}

void Http2Connection::close_outbound() noexcept {
    if (outbound_closed_) {
        return;
    }
    outbound_closed_ = true;

    for (Http2Stream *stream = owned_stream_list_.front(); stream != nullptr;
         stream = owned_stream_list_.next_of(*stream)) {
        if (stream->outbound_wait_state_ == Http2Stream::OutboundWaitState::ConnectionWindow) {
            remove_connection_window_wait(*stream);
        } else if (stream->outbound_wait_state_ == Http2Stream::OutboundWaitState::StreamWindow) {
            stream->outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
        } else {
            continue;
        }
        stream->outbound_kind_ = Http2OutboundKind::None;
    }

    if (outbound_idle()) {
        outbound_stopped_ = true;
    }
    schedule_io_pump();
}

void Http2Connection::abort_outbound(common::IoErr reason) noexcept {
    if (outbound_stopped_) {
        return;
    }
    outbound_stop_reason_ = reason != common::IoErr::None ? reason : common::IoErr::Canceled;
    outbound_closed_ = true;

    // TLS transports may retain pointers into this exact chain after
    // WouldBlock. Close the transport before releasing any in-flight buffers.
    if (!inflight_outbound_chain_.empty() && transport_ && transport_->valid()) {
        transport_->close();
    }

    while (Http2Stream *stream = connection_window_waiters_.front()) {
        connection_window_waiters_.erase(*stream);
        stream->outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
        stream->outbound_kind_ = Http2OutboundKind::None;
    }
    for (Http2Stream *stream = owned_stream_list_.front(); stream != nullptr;
         stream = owned_stream_list_.next_of(*stream)) {
        if (stream->outbound_wait_state_ == Http2Stream::OutboundWaitState::StreamWindow) {
            stream->outbound_wait_state_ = Http2Stream::OutboundWaitState::None;
            stream->outbound_kind_ = Http2OutboundKind::None;
        }
    }

    while (Http2OutboundHook *hook = outbound_queue_.front()) {
        outbound_queue_.erase(*hook);
        hook->encoded_.clear();
        hook->inflight_wire_bytes_ = 0;
        hook->window_consumed_ = 0;
        hook->completion_result_ = common::IoErr::None;
        hook->operation_final_batch_ = false;
        hook->state_ = Http2OutboundHook::State::Idle;
        if (hook->ctx_) {
            static_cast<Http2Stream *>(hook->ctx_)->outbound_kind_ = Http2OutboundKind::None;
        }
    }
    while (Http2OutboundHook *hook = inflight_outbound_hooks_.front()) {
        inflight_outbound_hooks_.erase(*hook);
        hook->encoded_.clear();
        hook->inflight_wire_bytes_ = 0;
        hook->window_consumed_ = 0;
        hook->completion_result_ = common::IoErr::None;
        hook->operation_final_batch_ = false;
        hook->state_ = Http2OutboundHook::State::Idle;
        if (hook->ctx_) {
            static_cast<Http2Stream *>(hook->ctx_)->outbound_kind_ = Http2OutboundKind::None;
        }
    }
    inflight_outbound_chain_.clear();
    control_hook_.encoded_.clear();
    outbound_stopped_ = true;
}

void Http2Connection::on_stream_outbound_idle(Http2Stream &stream) noexcept {
    if (stream.close_reason_ == common::IoErr::None && stream.outbound_kind_ != Http2OutboundKind::None) {
        common::IoErr err = try_encode_stream_outbound(stream);
        if (err != common::IoErr::None) {
            enter_closing(err);
            return;
        }
    }
    try_release_stream(stream);
}

void Http2Connection::clear_inbound_stream() noexcept {
    inbound_stream_.lease.reset();
    inbound_stream_.stream_id = 0;
    inbound_stream_.payload_begin = 0;
    inbound_stream_.payload_end = 0;
    inbound_stream_.header_block_open = false;
    inbound_stream_.end_stream_pending = false;
}

Http2Stream::Lease Http2Connection::alloc_peer_stream(std::uint32_t stream_id) noexcept {
    return peer_stream_factory_ops_.create_peer_stream(peer_stream_factory_ctx_, stream_id, *this);
}

void Http2Connection::close_all_streams(common::IoErr result) noexcept {
    for (Http2Stream *stream = owned_stream_list_.front(); stream != nullptr;) {
        Http2Stream *next = owned_stream_list_.next_of(*stream);
        Http2Stream::Lease held = stream->lease();
        stream->close(result);
        try_release_stream(*stream);
        stream = next;
    }
}

void Http2Connection::maybe_enter_closing_from_draining() noexcept {
    if (stop_sending_requested_ || state_ != State::Draining) {
        return;
    }
    if (streams_.size() != 0) {
        return;
    }
    if (!local_goaway_sent_) {
        return;
    }

    state_ = State::Closing;
    close_flush_outbound_ = true;
    stop_sending_requested_ = true;
    stop_sending_reason_ = common::IoErr::Canceled;
    clear_inbound_stream();
    close_all_streams(common::IoErr::Canceled);
    close_outbound();
    schedule_io_pump();
}

} // namespace fiber::http
