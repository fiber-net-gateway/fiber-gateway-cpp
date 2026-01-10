#ifndef FIBER_EVENT_SIGNAL_SERVICE_H
#define FIBER_EVENT_SIGNAL_SERVICE_H

#include <array>
#include <atomic>
#include <deque>
#include <stop_token>
#include <thread>

#include "../async/Signal.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "EventLoop.h"

namespace fiber::event {

class SignalService : public common::NonCopyable, public common::NonMovable {
public:
    explicit SignalService(EventLoop &loop);
    ~SignalService();

    bool attach(const fiber::async::SignalSet &mask);
    void detach();

    static SignalService &current();
    static SignalService *current_or_null() noexcept;

    void enqueue_waiter(int signum, fiber::async::detail::SignalWaiter *waiter);
    void cancel_waiter(fiber::async::detail::SignalWaiter *waiter);
    bool try_pop_pending(int signum, fiber::async::SignalInfo &out);

private:
    struct Delivery {
        EventLoop::DeferEntry entry{};
        SignalService *service = nullptr;
        fiber::async::SignalInfo info{};

        static void on_run(Delivery *self);
        static void on_cancel(Delivery *self);
    };

    struct WaiterQueue {
        fiber::async::detail::SignalWaiter *head = nullptr;
        fiber::async::detail::SignalWaiter *tail = nullptr;
    };

    static bool valid_signum(int signum) noexcept;
    fiber::async::detail::SignalWaiter *pop_next_waiter(int signum);
    void on_delivery(const fiber::async::SignalInfo &info);
    void run_dispatcher(std::stop_token stop_token);

    EventLoop &loop_;
    fiber::async::SignalSet mask_{};
    std::jthread dispatcher_{};
    std::atomic<bool> attached_{false};

    std::array<WaiterQueue, NSIG> waiters_{};
    std::array<std::deque<fiber::async::SignalInfo>, NSIG> pending_{};

    static thread_local SignalService *current_;
};

} // namespace fiber::event

#endif // FIBER_EVENT_SIGNAL_SERVICE_H
