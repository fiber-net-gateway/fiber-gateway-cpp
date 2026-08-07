#ifndef FIBER_DNS_DETAIL_DNS_UDP_SEND_QUEUE_H
#define FIBER_DNS_DETAIL_DNS_UDP_SEND_QUEUE_H

#include <coroutine>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"

namespace fiber::dns::detail {

// Serializes only the slow path after UdpSocket::try_send_to() reports WouldBlock.
// All operations must run on the bound EventLoop.
class DnsUdpSendQueue : public common::NonCopyable, public common::NonMovable {
public:
    class Owner;
    class AcquireAwaiter;

    DnsUdpSendQueue() noexcept = default;
    ~DnsUdpSendQueue();

    void init(event::EventLoop &loop) noexcept;
    void close(common::IoErr reason = common::IoErr::Canceled) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool fast_path_available() const noexcept;
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] Owner take_ownership_after_would_block() noexcept;
    [[nodiscard]] AcquireAwaiter acquire() noexcept;

    class Owner {
    public:
        Owner() noexcept = default;
        Owner(const Owner &) = delete;
        Owner &operator=(const Owner &) = delete;
        Owner(Owner &&other) noexcept;
        Owner &operator=(Owner &&other) noexcept;
        ~Owner();

        void release() noexcept;
        [[nodiscard]] bool owns() const noexcept { return queue_ != nullptr; }

    private:
        explicit Owner(DnsUdpSendQueue &queue) noexcept : queue_(&queue) {}

        DnsUdpSendQueue *queue_ = nullptr;

        friend class DnsUdpSendQueue;
        friend class AcquireAwaiter;
    };

    class AcquireAwaiter {
    public:
        explicit AcquireAwaiter(DnsUdpSendQueue &queue) noexcept : queue_(&queue) {}
        AcquireAwaiter(const AcquireAwaiter &) = delete;
        AcquireAwaiter &operator=(const AcquireAwaiter &) = delete;
        AcquireAwaiter(AcquireAwaiter &&) = delete;
        AcquireAwaiter &operator=(AcquireAwaiter &&) = delete;
        ~AcquireAwaiter();

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        common::IoResult<Owner> await_resume() noexcept;

    private:
        DnsUdpSendQueue *queue_ = nullptr;
        AcquireAwaiter *prev_ = nullptr;
        AcquireAwaiter *next_ = nullptr;
        std::coroutine_handle<> handle_{};
        common::IoErr err_ = common::IoErr::None;
        bool queued_ = false;
        bool granted_ = false;

        friend class DnsUdpSendQueue;
    };

private:
    enum class State : std::uint8_t {
        Idle,
        Owned,
        Handoff,
    };

    void enqueue(AcquireAwaiter &awaiter) noexcept;
    void unlink(AcquireAwaiter &awaiter) noexcept;
    [[nodiscard]] AcquireAwaiter *pop_front() noexcept;
    void finish_owner() noexcept;
    void schedule_handoff() noexcept;
    static void on_handoff(DnsUdpSendQueue *queue) noexcept;

    event::EventLoop *loop_ = nullptr;
    AcquireAwaiter *head_ = nullptr;
    AcquireAwaiter *tail_ = nullptr;
    event::EventLoop::DeferEntry handoff_entry_{};
    State state_ = State::Idle;
    bool closed_ = false;
};

} // namespace fiber::dns::detail

#endif // FIBER_DNS_DETAIL_DNS_UDP_SEND_QUEUE_H
