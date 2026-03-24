#ifndef FIBER_HTTP_HTTP2_OUTBOUND_SCHEDULER_H
#define FIBER_HTTP_HTTP2_OUTBOUND_SCHEDULER_H

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <utility>

#include "../async/Task.h"
#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/mem/IoBuf.h"
#include "../event/EventLoop.h"
#include "Http2OutboundHook.h"
#include "Http2Stream.h"

namespace fiber::http {

class HttpTransport;

struct Http2OutboundEncodeRequest {
    std::uint32_t max_frame_size = 0;
    std::int32_t conn_window_budget = 0;
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

class Http2OutboundPayloadStorage {
public:
    [[nodiscard]] common::IoErr append_uninitialized(std::size_t bytes, std::uint8_t *&dst) noexcept;
    [[nodiscard]] common::IoErr append_copy(const void *src, std::size_t bytes) noexcept;
    [[nodiscard]] common::IoErr append_buffer(mem::IoBuf &&buf) noexcept;
    [[nodiscard]] bool empty() const noexcept { return chain_.empty(); }
    [[nodiscard]] std::size_t readable_bytes() const noexcept { return chain_.readable_bytes(); }
    void clear() noexcept { chain_.clear(); }

private:
    [[nodiscard]] mem::IoBufChain take_chain() noexcept { return std::move(chain_); }

    mem::IoBufChain chain_{};

    friend class Http2OutboundScheduler;
};

class Http2OutboundScheduler {
public:
    explicit Http2OutboundScheduler(HttpTransport *transport = nullptr,
                                    std::size_t slab_capacity = 1024,
                                    std::chrono::milliseconds write_timeout = std::chrono::seconds(30),
                                    std::uint32_t peer_max_frame_size = 16384) noexcept;
    Http2OutboundScheduler(const Http2OutboundScheduler &) = delete;
    Http2OutboundScheduler &operator=(const Http2OutboundScheduler &) = delete;
    ~Http2OutboundScheduler();

    void set_transport(HttpTransport *transport) noexcept { transport_ = transport; }
    [[nodiscard]] HttpTransport *transport() const noexcept { return transport_; }

    void set_write_timeout(std::chrono::milliseconds timeout) noexcept { write_timeout_ = timeout; }
    [[nodiscard]] std::chrono::milliseconds write_timeout() const noexcept { return write_timeout_; }

    void set_peer_max_frame_size(std::uint32_t value) noexcept { peer_max_frame_size_ = value; }
    [[nodiscard]] std::uint32_t peer_max_frame_size() const noexcept { return peer_max_frame_size_; }

    void set_connection_send_window(std::int32_t value) noexcept { conn_send_window_ = value; }
    [[nodiscard]] std::int32_t connection_send_window() const noexcept { return conn_send_window_; }

    template<typename Encoder>
    [[nodiscard]] common::IoErr alloc_and_enqueue_control(std::size_t bytes, Encoder &&encoder) noexcept {
        bind_owner_loop_if_needed();

        Reservation reservation;
        common::IoErr err = reserve_tail(bytes, reservation);
        if (err != common::IoErr::None) {
            return err;
        }

        err = invoke_control_encoder(reservation.data, bytes, std::forward<Encoder>(encoder));
        if (err != common::IoErr::None) {
            rollback_reservation(reservation);
            return err;
        }

        commit_reservation(reservation);
        notify_waiter();
        return common::IoErr::None;
    }

    [[nodiscard]] common::IoErr request_send(Http2Stream &stream, Http2OutboundNextKind next_kind,
                                             Http2OutboundEncodeFn encode, void *ctx) noexcept;
    void cancel_stream(Http2Stream &stream) noexcept;
    void on_connection_window_available() noexcept;

    fiber::async::Task<void> send_loop() noexcept;

    void close() noexcept;
    void abort(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] bool aborting() const noexcept { return aborting_; }
    [[nodiscard]] bool send_loop_running() const noexcept { return send_loop_running_; }
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] std::size_t pending_control_bytes() const noexcept { return pending_control_bytes_; }
    [[nodiscard]] std::size_t slab_capacity() const noexcept { return slab_capacity_; }
    [[nodiscard]] std::size_t active_slab_count() const noexcept;
    [[nodiscard]] bool has_cached_slab() const noexcept { return cached_empty_slab_ != nullptr; }
    [[nodiscard]] std::size_t ready_stream_count() const noexcept { return ready_stream_count_; }
    [[nodiscard]] std::size_t waiting_conn_window_stream_count() const noexcept {
        return waiting_conn_window_stream_count_;
    }
    [[nodiscard]] bool has_inflight_stream_write() const noexcept { return inflight_stream_ != nullptr; }
    [[nodiscard]] common::IoErr stop_reason() const noexcept { return stop_reason_; }

private:
    struct Slab;

    class WaitForWorkAwaiter {
    public:
        explicit WaitForWorkAwaiter(Http2OutboundScheduler &queue) noexcept : queue_(&queue) {}
        WaitForWorkAwaiter(const WaitForWorkAwaiter &) = delete;
        WaitForWorkAwaiter &operator=(const WaitForWorkAwaiter &) = delete;
        WaitForWorkAwaiter(WaitForWorkAwaiter &&) = delete;
        WaitForWorkAwaiter &operator=(WaitForWorkAwaiter &&) = delete;
        ~WaitForWorkAwaiter();

        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        void await_resume() noexcept;

    private:
        static void on_notify(WaitForWorkAwaiter *awaiter);
        void resume() noexcept;

        Http2OutboundScheduler *queue_ = nullptr;
        fiber::event::EventLoop *loop_ = nullptr;
        std::coroutine_handle<> handle_{};
        fiber::event::EventLoop::NotifyEntry notify_entry_{};
        bool resume_posted_ = false;

        friend class Http2OutboundScheduler;
    };

    enum class QueueState : std::uint8_t {
        None = 0,
        Ready,
        WaitingConnWindow,
        Inflight,
    };

    struct Reservation {
        Slab *slab = nullptr;
        std::uint8_t *data = nullptr;
        std::size_t begin = 0;
        std::size_t bytes = 0;
    };

    struct SendSpan {
        const std::uint8_t *data = nullptr;
        std::size_t length = 0;
    };

    struct InflightStreamWrite {
        Http2Stream *stream = nullptr;
        mem::IoBufChain bytes{};

        [[nodiscard]] bool empty() const noexcept { return bytes.readable_bytes() == 0; }

        void clear() noexcept {
            stream = nullptr;
            bytes.clear();
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

    void bind_owner_loop_if_needed() noexcept;
    [[nodiscard]] common::IoErr reserve_tail(std::size_t bytes, Reservation &reservation) noexcept;
    void rollback_reservation(const Reservation &reservation) noexcept;
    void commit_reservation(const Reservation &reservation) noexcept;
    [[nodiscard]] WaitForWorkAwaiter wait_for_work() noexcept { return WaitForWorkAwaiter(*this); }
    [[nodiscard]] bool arm_waiter(WaitForWorkAwaiter *awaiter) noexcept;
    void cancel_waiter(WaitForWorkAwaiter *awaiter) noexcept;
    void notify_waiter() noexcept;
    [[nodiscard]] bool should_wake_waiter() const noexcept;
    [[nodiscard]] Slab *acquire_slab() noexcept;
    void recycle_slab(Slab *slab) noexcept;
    void append_tail_slab(Slab *slab) noexcept;
    void discard_empty_head_slabs() noexcept;
    [[nodiscard]] SendSpan current_send_span() noexcept;
    void consume_written_control_bytes(std::size_t bytes) noexcept;
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
    void finish_inflight_stream_write() noexcept;

    HttpTransport *transport_ = nullptr;
    std::chrono::milliseconds write_timeout_{};
    std::size_t slab_capacity_ = 0;
    std::uint32_t peer_max_frame_size_ = 0;
    std::int32_t conn_send_window_ = 0;
    std::size_t pending_control_bytes_ = 0;
    std::size_t sending_end_ = 0;
    Slab *head_slab_ = nullptr;
    Slab *tail_slab_ = nullptr;
    Slab *cached_empty_slab_ = nullptr;
    StreamList ready_streams_{};
    StreamList waiting_conn_window_streams_{};
    std::size_t ready_stream_count_ = 0;
    std::size_t waiting_conn_window_stream_count_ = 0;
    InflightStreamWrite inflight_stream_write_{};
    WaitForWorkAwaiter *waiter_ = nullptr;
    fiber::event::EventLoop *owner_loop_ = nullptr;
    bool closed_ = false;
    bool aborting_ = false;
    bool send_loop_running_ = false;
    common::IoErr stop_reason_ = common::IoErr::None;
    Http2Stream *inflight_stream_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_SCHEDULER_H
