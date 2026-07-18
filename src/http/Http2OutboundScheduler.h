#ifndef FIBER_HTTP_HTTP2_OUTBOUND_SCHEDULER_H
#define FIBER_HTTP_HTTP2_OUTBOUND_SCHEDULER_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "../event/EventLoop.h"
#include "Http2OutboundHook.h"
#include "Http2Stream.h"

namespace fiber::http {

class HttpTransport;

struct Http2OutboundEncodeRequest {
    std::uint32_t max_frame_size = 0;
    std::int32_t conn_window_budget = 0;
};

using Http2OutboundDoneFn = void (*)(void *ctx, common::IoErr result) noexcept;
using Http2OutboundWakeFn = void (*)(void *ctx) noexcept;

struct Http2OutboundPumpResult {
    event::IoEvent wait_event = event::IoEvent::None;
    std::size_t bytes_written = 0;
    bool needs_reschedule = false;
    bool stopped = false;
};

struct Http2OutboundEncodeResult {
    enum class Status : std::uint8_t {
        Encoded = 0,
        NoWork,
        BlockedConnWindow,
        BlockedStreamWindow,
        Closed,
    };

    Status status = Status::NoWork;
    Http2OutboundNextKind next_kind = Http2OutboundNextKind::None;
    std::uint32_t consumed_conn_window = 0;
};

class Http2OutboundEncodeTarget {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t total_bytes() const noexcept;

    [[nodiscard]] common::IoErr append_copy(const void *src, std::size_t bytes) noexcept;
    [[nodiscard]] common::IoErr append_buffer(mem::IoBuf &&buf) noexcept;
    [[nodiscard]] common::IoErr append_chain(mem::IoBufChain &&chain) noexcept;
    void set_on_done(Http2OutboundDoneFn fn, void *ctx) noexcept;

private:
    void reset(mem::IoBufNodePool &node_pool) noexcept;
    [[nodiscard]] mem::IoBufChain take_chain() noexcept { return std::move(chain_); }
    [[nodiscard]] Http2OutboundDoneFn done_fn() const noexcept { return done_fn_; }
    [[nodiscard]] void *done_ctx() const noexcept { return done_ctx_; }

    mem::IoBufChain chain_{};
    Http2OutboundDoneFn done_fn_ = nullptr;
    void *done_ctx_ = nullptr;

    friend class Http2OutboundScheduler;
};

class Http2OutboundScheduler {
public:
    explicit Http2OutboundScheduler(std::uint32_t peer_max_frame_size = 16384) noexcept;
    Http2OutboundScheduler(const Http2OutboundScheduler &) = delete;
    Http2OutboundScheduler &operator=(const Http2OutboundScheduler &) = delete;
    ~Http2OutboundScheduler();

    void bind_owner_loop(event::EventLoop &loop) noexcept;

    void set_peer_max_frame_size(std::uint32_t value) noexcept { peer_max_frame_size_ = value; }
    [[nodiscard]] std::uint32_t peer_max_frame_size() const noexcept { return peer_max_frame_size_; }

    void set_connection_send_window(std::int32_t value) noexcept { conn_send_window_ = value; }
    [[nodiscard]] std::int32_t connection_send_window() const noexcept { return conn_send_window_; }

    template<typename Encoder>
    [[nodiscard]] common::IoErr alloc_and_enqueue_control(std::size_t bytes, Encoder &&encoder) noexcept {
        if (!bind_owner_loop_if_needed()) {
            return common::IoErr::Invalid;
        }
        if (bytes == 0) {
            return common::IoErr::Invalid;
        }
        if (stop_reason_ != common::IoErr::None) {
            return stop_reason_;
        }
        if (closed_) {
            return common::IoErr::Canceled;
        }
        mem::IoBuf buf = mem::IoBuf::allocate(bytes);
        if (!buf) {
            return common::IoErr::NoMem;
        }
        common::IoErr err = invoke_control_encoder(buf.writable_data(), bytes, std::forward<Encoder>(encoder));
        if (err != common::IoErr::None) {
            return err;
        }
        buf.commit(bytes);
        if (!control_chain_.append(std::move(buf))) {
            return common::IoErr::NoMem;
        }
        notify_work();
        return common::IoErr::None;
    }

    [[nodiscard]] common::IoErr request_send(Http2Stream &stream, Http2OutboundNextKind next_kind,
                                             Http2OutboundEncodeFn encode, void *ctx) noexcept;
    [[nodiscard]] bool cancel_queued_send(Http2Stream &stream) noexcept;
    void cancel_stream(Http2Stream &stream) noexcept;
    void on_connection_window_available() noexcept;

    void set_wake_callback(Http2OutboundWakeFn callback, void *ctx) noexcept {
        wake_callback_ = callback;
        wake_ctx_ = callback ? ctx : nullptr;
    }

    [[nodiscard]] common::IoResult<Http2OutboundPumpResult>
    pump_write(HttpTransport &transport, std::size_t operation_budget, std::size_t byte_budget) noexcept;

    void close() noexcept;
    void abort(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] bool aborting() const noexcept { return aborting_; }
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] std::size_t pending_control_bytes() const noexcept { return control_chain_.readable_bytes(); }
    [[nodiscard]] std::size_t ready_stream_count() const noexcept { return ready_stream_count_; }
    [[nodiscard]] std::size_t waiting_conn_window_stream_count() const noexcept {
        return waiting_conn_window_stream_count_;
    }
    [[nodiscard]] bool has_inflight_stream_write() const noexcept { return inflight_stream_ != nullptr; }
    [[nodiscard]] common::IoErr stop_reason() const noexcept { return stop_reason_; }

private:
    enum class QueueState : std::uint8_t {
        None = 0,
        Ready,
        WaitingConnWindow,
        Inflight,
    };

    struct InflightStreamWrite {
        mem::IoBufChain chain{};
        Http2OutboundDoneFn done = nullptr;
        void *done_ctx = nullptr;

        [[nodiscard]] bool empty() const noexcept { return chain.readable_bytes() == 0; }

        void clear() noexcept {
            chain.clear();
            done = nullptr;
            done_ctx = nullptr;
        }
    };

    template<typename T>
    static constexpr bool kAlwaysFalse = false;

    template<typename Encoder>
    [[nodiscard]] static common::IoErr invoke_control_encoder(std::uint8_t *dst, std::size_t bytes,
                                                              Encoder &&encoder) noexcept {
        if constexpr (std::invocable<Encoder &, std::uint8_t *, std::size_t>) {
            using Result = std::invoke_result_t<Encoder &, std::uint8_t *, std::size_t>;
            if constexpr (std::same_as<Result, void>) {
                std::invoke(encoder, dst, bytes);
                return common::IoErr::None;
            } else if constexpr (std::same_as<Result, bool>) {
                return std::invoke(encoder, dst, bytes) ? common::IoErr::None : common::IoErr::Invalid;
            } else if constexpr (std::same_as<Result, common::IoErr>) {
                return std::invoke(encoder, dst, bytes);
            } else {
                static_assert(kAlwaysFalse<Result>, "encoder must return void, bool, or common::IoErr");
            }
        } else if constexpr (std::invocable<Encoder &, std::uint8_t *>) {
            using Result = std::invoke_result_t<Encoder &, std::uint8_t *>;
            if constexpr (std::same_as<Result, void>) {
                std::invoke(encoder, dst);
                return common::IoErr::None;
            } else if constexpr (std::same_as<Result, bool>) {
                return std::invoke(encoder, dst) ? common::IoErr::None : common::IoErr::Invalid;
            } else if constexpr (std::same_as<Result, common::IoErr>) {
                return std::invoke(encoder, dst);
            } else {
                static_assert(kAlwaysFalse<Result>, "encoder must return void, bool, or common::IoErr");
            }
        } else {
            static_assert(kAlwaysFalse<Encoder>,
                          "encoder must be invocable as (std::uint8_t *) or (std::uint8_t *, std::size_t)");
        }
    }

    using StreamList = common::IntrusiveList<Http2Stream, offsetof(Http2Stream, outbound_hook_)>;

    [[nodiscard]] bool bind_owner_loop_if_needed() noexcept;
    void notify_work() noexcept;
    void clear_control_state() noexcept;
    void fail(common::IoErr reason) noexcept;
    void clear_ready_streams() noexcept;
    void clear_waiting_conn_window_streams() noexcept;
    void clear_all_stream_state() noexcept;
    [[nodiscard]] bool has_ready_work() const noexcept;
    void enqueue_ready(Http2Stream &stream) noexcept;
    void enqueue_waiting_conn_window(Http2Stream &stream) noexcept;
    void unlink_stream(Http2Stream &stream) noexcept;
    void classify_stream(Http2Stream &stream, bool notify) noexcept;
    [[nodiscard]] Http2Stream *pop_ready_stream() noexcept;
    [[nodiscard]] Http2Stream *pop_waiting_conn_window_stream() noexcept;
    void notify_inflight_done(common::IoErr result) noexcept;
    void finish_inflight_stream_write() noexcept;

    std::uint32_t peer_max_frame_size_ = 0;
    std::int32_t conn_send_window_ = 0;
    mem::IoBufChain control_chain_{};
    StreamList ready_streams_{};
    StreamList waiting_conn_window_streams_{};
    std::size_t ready_stream_count_ = 0;
    std::size_t waiting_conn_window_stream_count_ = 0;
    InflightStreamWrite inflight_stream_write_{};
    fiber::event::EventLoop *owner_loop_ = nullptr;
    Http2OutboundWakeFn wake_callback_ = nullptr;
    void *wake_ctx_ = nullptr;
    bool closed_ = false;
    bool aborting_ = false;
    bool stopped_ = false;
    common::IoErr stop_reason_ = common::IoErr::None;
    Http2Stream *inflight_stream_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_SCHEDULER_H
