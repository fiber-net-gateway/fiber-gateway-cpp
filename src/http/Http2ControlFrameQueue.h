#ifndef FIBER_HTTP_HTTP2_CONTROL_FRAME_QUEUE_H
#define FIBER_HTTP_HTTP2_CONTROL_FRAME_QUEUE_H

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <utility>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../event/EventLoop.h"

namespace fiber::http {

class HttpTransport;

// A single-loop outbound byte queue for small HTTP/2 control frames.
// Each alloc_and_enqueue reservation must fit inside one slab.
class Http2ControlFrameQueue {
public:
    explicit Http2ControlFrameQueue(HttpTransport *transport = nullptr,
                                    std::size_t slab_capacity = 1024,
                                    std::chrono::milliseconds write_timeout = std::chrono::seconds(30)) noexcept;
    Http2ControlFrameQueue(const Http2ControlFrameQueue &) = delete;
    Http2ControlFrameQueue &operator=(const Http2ControlFrameQueue &) = delete;
    ~Http2ControlFrameQueue();

    void set_transport(HttpTransport *transport) noexcept { transport_ = transport; }
    [[nodiscard]] HttpTransport *transport() const noexcept { return transport_; }

    void set_write_timeout(std::chrono::milliseconds timeout) noexcept { write_timeout_ = timeout; }
    [[nodiscard]] std::chrono::milliseconds write_timeout() const noexcept { return write_timeout_; }

    template<typename Encoder>
    [[nodiscard]] common::IoErr alloc_and_enqueue(std::size_t bytes, Encoder &&encoder) noexcept {
        bind_owner_loop_if_needed();

        Reservation reservation;
        common::IoErr err = reserve_tail(bytes, reservation);
        if (err != common::IoErr::None) {
            return err;
        }

        err = invoke_encoder(reservation.data, bytes, std::forward<Encoder>(encoder));
        if (err != common::IoErr::None) {
            rollback_reservation(reservation);
            return err;
        }

        commit_reservation(reservation);
        notify_waiter();
        return common::IoErr::None;
    }

    fiber::async::Task<void> send_loop() noexcept;

    void close() noexcept;
    void abort(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] bool aborting() const noexcept { return aborting_; }
    [[nodiscard]] bool send_loop_running() const noexcept { return send_loop_running_; }
    [[nodiscard]] bool idle() const noexcept { return pending_bytes_ == 0; }
    [[nodiscard]] std::size_t pending_bytes() const noexcept { return pending_bytes_; }
    [[nodiscard]] std::size_t slab_capacity() const noexcept { return slab_capacity_; }
    [[nodiscard]] std::size_t active_slab_count() const noexcept;
    [[nodiscard]] bool has_cached_slab() const noexcept { return cached_empty_slab_ != nullptr; }
    [[nodiscard]] common::IoErr stop_reason() const noexcept { return stop_reason_; }

private:
    struct Slab;

    class WaitForDataAwaiter {
    public:
        explicit WaitForDataAwaiter(Http2ControlFrameQueue &queue) noexcept : queue_(&queue) {}
        WaitForDataAwaiter(const WaitForDataAwaiter &) = delete;
        WaitForDataAwaiter &operator=(const WaitForDataAwaiter &) = delete;
        WaitForDataAwaiter(WaitForDataAwaiter &&) = delete;
        WaitForDataAwaiter &operator=(WaitForDataAwaiter &&) = delete;
        ~WaitForDataAwaiter();

        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        void await_resume() noexcept;

    private:
        static void on_notify(WaitForDataAwaiter *awaiter);
        void resume() noexcept;

        Http2ControlFrameQueue *queue_ = nullptr;
        fiber::event::EventLoop *loop_ = nullptr;
        std::coroutine_handle<> handle_{};
        fiber::event::EventLoop::NotifyEntry notify_entry_{};
        bool resume_posted_ = false;

        friend class Http2ControlFrameQueue;
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

    template<typename T>
    static constexpr bool kAlwaysFalse = false;

    template<typename Encoder>
    [[nodiscard]] static common::IoErr invoke_encoder(std::uint8_t *dst, std::size_t bytes, Encoder &&encoder) noexcept {
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

    void bind_owner_loop_if_needed() noexcept;
    [[nodiscard]] common::IoErr reserve_tail(std::size_t bytes, Reservation &reservation) noexcept;
    void rollback_reservation(const Reservation &reservation) noexcept;
    void commit_reservation(const Reservation &reservation) noexcept;
    [[nodiscard]] WaitForDataAwaiter wait_for_data() noexcept { return WaitForDataAwaiter(*this); }
    [[nodiscard]] bool arm_waiter(WaitForDataAwaiter *awaiter) noexcept;
    void cancel_waiter(WaitForDataAwaiter *awaiter) noexcept;
    void notify_waiter() noexcept;
    [[nodiscard]] bool should_wake_waiter() const noexcept;
    [[nodiscard]] Slab *acquire_slab() noexcept;
    void recycle_slab(Slab *slab) noexcept;
    void append_tail_slab(Slab *slab) noexcept;
    void discard_empty_head_slabs() noexcept;
    [[nodiscard]] SendSpan current_send_span() noexcept;
    void consume_written_bytes(std::size_t bytes) noexcept;
    void fail(common::IoErr reason) noexcept;

    HttpTransport *transport_ = nullptr;
    std::chrono::milliseconds write_timeout_{};
    std::size_t slab_capacity_ = 0;
    std::size_t pending_bytes_ = 0;
    std::size_t sending_end_ = 0;
    Slab *head_slab_ = nullptr;
    Slab *tail_slab_ = nullptr;
    Slab *cached_empty_slab_ = nullptr;
    WaitForDataAwaiter *waiter_ = nullptr;
    fiber::event::EventLoop *owner_loop_ = nullptr;
    bool closed_ = false;
    bool aborting_ = false;
    bool send_loop_running_ = false;
    common::IoErr stop_reason_ = common::IoErr::None;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CONTROL_FRAME_QUEUE_H
