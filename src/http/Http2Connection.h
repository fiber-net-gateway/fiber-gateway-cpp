#ifndef FIBER_HTTP_HTTP2_CONNECTION_H
#define FIBER_HTTP_HTTP2_CONNECTION_H

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "../async/Task.h"
#include "../common/Assert.h"
#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "Http2HpackDecoder.h"
#include "Http2HpackEncodeCatalog.h"
#include "Http2HpackEncoder.h"
#include "Http2OutboundHook.h"
#include "Http2OutboundScheduler.h"
#include "Http2Protocol.h"
#include "Http2Stream.h"
#include "Http2StreamFactory.h"
#include "Http2StreamTable.h"
#include "HttpTransport.h"

namespace fiber::http {

class ServerHttp2Request;

class Http2Connection : public common::NonCopyable, public common::NonMovable {
public:
    enum class ConnectionRole : std::uint8_t {
        Client,
        Server,
    };

    enum class State : std::uint8_t {
        Init,
        Start,
        Running,
        Draining,
        Closing,
        Closed,
    };

    using FrameHeader = Http2FrameHeader;
    using RunResult = common::IoResult<void>;
    using ClosedCallback = void (*)(void *ctx, Http2Connection &connection, RunResult result) noexcept;

    struct Options {
        ConnectionRole role = ConnectionRole::Server;
        const Http2HpackEncodeCatalog *outbound_hpack_catalog = nullptr;
        std::size_t read_buffer_size = 64 * 1024;
        std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
        // Retain an empty unique read buffer for this long after the last
        // successful inbound read. Zero disables idle buffer release.
        std::chrono::milliseconds read_buffer_idle_release_timeout = std::chrono::milliseconds::zero();
        std::chrono::milliseconds write_timeout = std::chrono::seconds(30);
        std::chrono::milliseconds keepalive_ping_interval = std::chrono::milliseconds::zero();
        std::uint32_t max_frame_size = 16384;
        std::uint32_t max_hpack_string_size = 64 * 1024;
        std::size_t max_free_send_entries = 64;
        // Initial peer-advertised SETTINGS_MAX_CONCURRENT_STREAMS budget used
        // for streams we create until the peer sends its own SETTINGS frame.
        std::uint32_t max_peer_concurrent_streams = 100;
        // SETTINGS_MAX_CONCURRENT_STREAMS that we advertise to the peer and
        // enforce for peer-created streams on this connection.
        std::uint32_t local_max_concurrent_streams = 128;
        std::uint32_t max_local_push_streams = 0;
        std::uint32_t initial_connection_recv_window = 0x7fffffffU;
        std::uint32_t connection_recv_window_low_watermark = 16 * 1024 * 1024;
        std::uint32_t initial_stream_recv_window = 64 * 1024;
        std::uint32_t stream_recv_window_low_watermark = 16 * 1024;
        std::int32_t initial_connection_send_window = 65535;
        std::int32_t initial_stream_send_window = 65535;
        bool enable_connect_protocol = false;
    };

    virtual ~Http2Connection();

    Http2Connection(Options options, void *peer_stream_factory_ctx,
                    const Http2StreamFactoryOps &peer_stream_factory_ops);

    // Must be called on transport->loop(). A successful start owns and drives
    // transport I/O until closure; no run coroutine is required. on_closed is
    // invoked on the loop after all connection state is closed and may destroy
    // the connection. Use either on_closed or run()/wait_closed(), not both.
    common::IoErr start(std::unique_ptr<HttpTransport> transport, ClosedCallback on_closed = nullptr,
                        void *closed_ctx = nullptr) noexcept;

    // Compatibility waiters; these do not drive I/O.
    fiber::async::Task<RunResult> run() noexcept;
    fiber::async::Task<RunResult> wait_closed() noexcept;
    [[nodiscard]] common::IoResult<Http2Stream::Lease> attach_local_stream(Http2Stream &stream) noexcept;
    void shutdown(common::IoErr reason = common::IoErr::Canceled) noexcept;
    void graceful_shutdown() noexcept;
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool peer_settings_received() const noexcept { return peer_settings_received_; }
    [[nodiscard]] bool peer_enable_connect_protocol() const noexcept { return peer_enable_connect_protocol_; }
    [[nodiscard]] event::EventLoop &loop() noexcept {
        FIBER_ASSERT(transport_ != nullptr);
        return transport_->loop();
    }
    [[nodiscard]] const event::EventLoop &loop() const noexcept {
        FIBER_ASSERT(transport_ != nullptr);
        return transport_->loop();
    }
    [[nodiscard]] HttpTransport &transport() noexcept {
        FIBER_ASSERT(transport_ != nullptr);
        return *transport_;
    }
    [[nodiscard]] const HttpTransport &transport() const noexcept {
        FIBER_ASSERT(transport_ != nullptr);
        return *transport_;
    }

protected:
    // `offset` is the number of payload bytes already delivered for the current
    // frame. Only the first `length` bytes starting at `buf.readable_data()`
    // are part of this callback's payload chunk.
    virtual common::IoErr on_frame_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                           std::size_t length) noexcept;

    Http2Stream *find_stream(std::uint32_t stream_id) noexcept;
    const Http2Stream *find_stream(std::uint32_t stream_id) const noexcept;
    void update_connection_send_window(std::int32_t delta) noexcept;
    void stop_sending(common::IoErr reason = common::IoErr::Canceled) noexcept;
    [[nodiscard]] std::int32_t connection_send_window() const noexcept {
        return outbound_scheduler_.connection_send_window();
    }
    [[nodiscard]] std::uint32_t peer_max_outbound_frame_size() const noexcept { return peer_max_outbound_frame_size_; }
    [[nodiscard]] std::uint32_t peer_max_concurrent_streams() const noexcept {
        return peer_advertised_max_concurrent_streams_;
    }
    [[nodiscard]] ConnectionRole role() const noexcept { return options_.role; }
    [[nodiscard]] bool peer_enable_push() const noexcept { return peer_enable_push_; }
    [[nodiscard]] bool local_enable_connect_protocol() const noexcept { return options_.enable_connect_protocol; }
    [[nodiscard]] bool has_stream(std::uint32_t stream_id) const noexcept {
        return streams_.find(stream_id) != nullptr;
    }
    [[nodiscard]] bool outbound_stopped() const noexcept { return outbound_scheduler_.stopped(); }
    [[nodiscard]] Http2HpackDecoder &inbound_hpack_decoder() noexcept { return inbound_hpack_decoder_; }
    [[nodiscard]] const Http2HpackDecoder &inbound_hpack_decoder() const noexcept { return inbound_hpack_decoder_; }
    fiber::async::Task<void> stop_and_wait_closed(common::IoErr reason = common::IoErr::Canceled) noexcept;

private:
    enum class ParsePhase : std::uint8_t {
        Preface,
        FrameHeader,
        FramePayload,
    };

    struct InboundIoState {
        mem::IoBuf read_buf{};
        FrameHeader current_header{};
        std::chrono::steady_clock::time_point last_inbound_at{};
        std::chrono::steady_clock::time_point ping_sent_at{};
        std::uint32_t payload_remaining = 0;
        std::size_t payload_offset = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        ParsePhase phase = ParsePhase::FrameHeader;
        bool ready_hint = false;
        bool operation_pending = false;
    };

    struct ReadPumpResult {
        event::IoEvent wait_event = event::IoEvent::None;
        std::size_t bytes_read = 0;
        bool needs_reschedule = false;
    };

    struct ClosedAwaiter {
        Http2Connection *connection = nullptr;
        std::coroutine_handle<> handle{};

        ~ClosedAwaiter();

        [[nodiscard]] bool await_ready() const noexcept { return connection->close_completion_dispatched_; }
        bool await_suspend(std::coroutine_handle<> continuation) noexcept;
        void await_resume() noexcept;
    };

    struct InboundStream {
        Http2Stream::Lease lease{};
        std::uint32_t stream_id = 0;
        std::size_t payload_begin = 0;
        std::size_t payload_end = 0;
        bool header_block_open = false;
        bool end_stream_pending = false;
    };

    common::IoErr consume_incoming_frame_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                 std::size_t length) noexcept;
    common::IoErr handle_data_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                      std::size_t length) noexcept;
    common::IoErr handle_headers_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                         std::size_t length) noexcept;
    common::IoErr handle_continuation_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                              std::size_t length) noexcept;
    common::IoErr handle_settings_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                          std::size_t length) noexcept;
    common::IoErr handle_ping_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                      std::size_t length) noexcept;
    common::IoErr handle_window_update_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                               std::size_t length) noexcept;
    common::IoErr handle_rst_stream_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                            std::size_t length) noexcept;
    common::IoErr handle_goaway_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                        std::size_t length) noexcept;
    common::IoErr apply_settings_parameter(std::uint16_t id, std::uint32_t value) noexcept;
    common::IoErr apply_peer_initial_stream_window(std::uint32_t value) noexcept;
    common::IoErr send_initial_flight() noexcept;
    common::IoErr send_settings_ack() noexcept;
    common::IoErr send_ping_ack(const std::uint8_t *opaque_data) noexcept;
    common::IoErr send_window_update(std::uint32_t stream_id, std::uint32_t increment) noexcept;
    common::IoErr send_rst_stream(std::uint32_t stream_id, Http2ErrorCode error_code) noexcept;
    common::IoErr send_goaway(std::uint32_t last_stream_id, Http2ErrorCode error_code) noexcept;
    [[nodiscard]] common::IoErr maybe_replenish_connection_recv_window() noexcept;
    [[nodiscard]] std::uint32_t configured_initial_stream_recv_window() const noexcept;
    void handle_stream_error(std::uint32_t stream_id, Http2ErrorCode error_code,
                             common::IoErr pending_result = common::IoErr::Canceled) noexcept;
    Http2Stream *create_peer_stream(std::uint32_t stream_id) noexcept;
    void detach_stream(Http2Stream &stream) noexcept;
    void try_release_stream(Http2Stream &stream) noexcept;
    bool can_accept_peer_stream(std::uint32_t stream_id) const noexcept;
    bool can_attach_local_stream() const noexcept;
    bool is_next_peer_stream_id(std::uint32_t stream_id) const noexcept;
    void handle_peer_goaway(std::uint32_t last_stream_id, Http2ErrorCode error_code) noexcept;
    void close_streams_after_goaway(std::uint32_t last_stream_id) noexcept;
    [[nodiscard]] bool is_idle_stream(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool is_local_stream_id(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool is_peer_stream_id(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::size_t configured_max_active_streams() const noexcept;
    [[nodiscard]] common::IoErr request_stream_send(Http2Stream &stream, Http2OutboundNextKind next_kind) noexcept;
    [[nodiscard]] bool cancel_queued_stream_send(Http2Stream &stream) noexcept;
    void cancel_stream_send(Http2Stream &stream) noexcept;
    [[nodiscard]] common::IoResult<ReadPumpResult> pump_read(std::size_t operation_budget,
                                                             std::size_t byte_budget) noexcept;
    [[nodiscard]] common::IoErr consume_read_buffer(std::size_t &operation_budget, std::size_t &byte_budget) noexcept;
    void handle_read_eof() noexcept;
    [[nodiscard]] std::chrono::milliseconds current_read_timeout() const noexcept;
    common::IoErr handle_read_timeout() noexcept;
    common::IoErr send_keepalive_ping() noexcept;
    common::IoErr start_client_session() noexcept;
    common::IoErr start_server_session() noexcept;
    void on_stream_outbound_idle(Http2Stream &stream) noexcept;
    static void on_transport_read_ready(void *ctx, common::IoErr err) noexcept;
    static void on_transport_write_ready(void *ctx, common::IoErr err) noexcept;
    static void on_outbound_work(void *ctx) noexcept;
    static void on_io_pump(Http2Connection *connection) noexcept;
    static void on_read_timer(Http2Connection *connection) noexcept;
    static void on_write_timer(Http2Connection *connection) noexcept;
    static void on_read_buffer_idle_timer(Http2Connection *connection) noexcept;
    static void on_closed_completion(Http2Connection *connection) noexcept;
    void handle_transport_ready(event::IoEvent event, common::IoErr err) noexcept;
    void schedule_io_pump() noexcept;
    void drive_io() noexcept;
    [[nodiscard]] common::IoErr sync_transport_callbacks() noexcept;
    void clear_transport_callbacks() noexcept;
    void arm_read_timer() noexcept;
    void arm_write_timer(bool made_progress) noexcept;
    void arm_read_buffer_idle_timer() noexcept;
    void cancel_io_timers() noexcept;
    void finish_connection() noexcept;
    void schedule_closed_completion() noexcept;
    void dispatch_closed_completion() noexcept;
    common::IoErr start_draining() noexcept;
    void maybe_enter_closing_from_draining() noexcept;
    void enter_closing(common::IoErr reason, bool report_error = true) noexcept;
    void close_all_streams(common::IoErr result) noexcept;
    void clear_inbound_stream() noexcept;
    [[nodiscard]] Http2Stream::Lease alloc_peer_stream(std::uint32_t stream_id) noexcept;
    [[nodiscard]] Http2HpackEncoder &outbound_hpack_encoder() noexcept { return outbound_hpack_encoder_; }
    [[nodiscard]] const Http2HpackEncoder &outbound_hpack_encoder() const noexcept { return outbound_hpack_encoder_; }

    std::unique_ptr<HttpTransport> transport_;
    Options options_;
    void *peer_stream_factory_ctx_ = nullptr;
    const Http2StreamFactoryOps peer_stream_factory_ops_{};
    Http2StreamTable streams_;
    Http2HpackDecoder inbound_hpack_decoder_;
    Http2HpackEncoder outbound_hpack_encoder_;
    std::uint32_t peer_advertised_max_concurrent_streams_ = 100;
    std::uint32_t last_peer_stream_id_ = 0;
    std::uint32_t last_local_stream_id_ = 0;
    std::uint32_t next_local_stream_id_ = 0;
    std::size_t peer_active_stream_count_ = 0;
    std::size_t local_active_stream_count_ = 0;
    std::int32_t conn_recv_window_remaining_ = 65535;
    std::uint32_t conn_recv_window_target_ = 65535;
    std::int32_t peer_initial_stream_send_window_ = 65535;
    std::uint32_t peer_header_table_size_ = 4096;
    std::uint32_t peer_max_outbound_frame_size_ = 16384;
    std::uint32_t peer_max_header_list_size_ = 0xffffffffU;
    bool peer_enable_push_ = true;
    bool peer_settings_received_ = false;
    bool peer_enable_connect_protocol_ = false;
    bool local_goaway_sent_ = false;
    std::uint32_t local_goaway_last_stream_id_ = 0;
    bool peer_goaway_received_ = false;
    std::uint32_t peer_last_stream_id_ = 0;
    Http2ErrorCode peer_goaway_error_code_ = Http2ErrorCode::NoError;
    InboundStream inbound_stream_{};
    std::array<std::uint8_t, 8> control_payload_scratch_{};
    std::size_t control_payload_used_ = 0;
    std::array<std::uint8_t, 6> settings_scratch_{};
    std::size_t settings_scratch_used_ = 0;
    std::array<std::uint8_t, 8> keepalive_ping_payload_{};
    std::uint64_t keepalive_ping_sequence_ = 0;
    bool keepalive_ping_outstanding_ = false;
    Http2OutboundScheduler outbound_scheduler_;
    InboundIoState inbound_io_{};
    common::IntrusiveList<Http2Stream, offsetof(Http2Stream, owned_hook_)> owned_stream_list_;
    event::EventLoop::NotifyEntry io_pump_entry_{};
    event::EventLoop::NotifyEntry close_completion_entry_{};
    event::EventLoop::TimerEntry read_timer_entry_{};
    event::EventLoop::TimerEntry write_timer_entry_{};
    event::EventLoop::TimerEntry read_buffer_idle_timer_entry_{};
    std::chrono::steady_clock::time_point write_blocked_at_{};
    ClosedCallback on_closed_ = nullptr;
    void *closed_ctx_ = nullptr;
    std::coroutine_handle<> closed_waiter_{};
    event::IoEvent outbound_wait_event_ = event::IoEvent::None;
    State state_ = State::Init;
    bool stop_sending_requested_ = false;
    bool physical_read_registered_ = false;
    bool physical_write_registered_ = false;
    bool io_pump_posted_ = false;
    bool io_pump_running_ = false;
    bool io_pump_again_ = false;
    bool prefer_write_ = false;
    bool outbound_ready_hint_ = false;
    bool inbound_eof_ = false;
    bool close_flush_outbound_ = false;
    bool close_finished_ = false;
    bool close_completion_posted_ = false;
    bool close_completion_dispatched_ = false;
    common::IoErr stop_sending_reason_ = common::IoErr::Canceled;
    common::IoErr terminal_error_ = common::IoErr::None;

    friend class Http2Stream;
    friend class Http2OutboundScheduler;
    friend class ServerHttp2Request;
    friend class ClientHttp2Request;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CONNECTION_H
