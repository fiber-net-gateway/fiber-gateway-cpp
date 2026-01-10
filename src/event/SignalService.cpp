#include "SignalService.h"

#include <cerrno>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "../common/Assert.h"

namespace fiber::event {

thread_local SignalService *SignalService::current_ = nullptr;

namespace {

fiber::async::SignalInfo to_signal_info(const siginfo_t &info) {
    fiber::async::SignalInfo out{};
    out.signum = info.si_signo;
    out.code = info.si_code;
    out.pid = info.si_pid;
    out.uid = info.si_uid;
    out.status = info.si_status;
    out.errno_ = info.si_errno;
    out.value = reinterpret_cast<std::intptr_t>(info.si_value.sival_ptr);
    return out;
}

} // namespace

SignalService::SignalService(EventLoop &loop) : loop_(loop) {
}

SignalService::~SignalService() {
    if (attached_.load(std::memory_order_acquire) && loop_.in_loop()) {
        detach();
    }
    if (dispatcher_.joinable()) {
        dispatcher_.request_stop();
        dispatcher_.join();
    }
    if (current_ == this) {
        current_ = nullptr;
    }
}

bool SignalService::attach(const fiber::async::SignalSet &mask) {
    FIBER_ASSERT(loop_.in_loop());
    if (attached_.load(std::memory_order_acquire)) {
        return false;
    }
    FIBER_ASSERT(current_ == nullptr);
    mask_ = mask;
    pthread_sigmask(SIG_BLOCK, &mask_.native(), nullptr);
    attached_.store(true, std::memory_order_release);
    current_ = this;
    dispatcher_ = std::jthread([this](std::stop_token stop_token) {
        run_dispatcher(stop_token);
    });
    return true;
}

void SignalService::detach() {
    FIBER_ASSERT(loop_.in_loop());
    if (!attached_.load(std::memory_order_acquire)) {
        return;
    }
    attached_.store(false, std::memory_order_release);
    if (dispatcher_.joinable()) {
        dispatcher_.request_stop();
        dispatcher_.join();
    }
    for (int signum = 0; signum < NSIG; ++signum) {
        auto &queue = waiters_[signum];
        FIBER_ASSERT(queue.head == nullptr);
        queue.tail = nullptr;
        pending_[signum].clear();
    }
    if (current_ == this) {
        current_ = nullptr;
    }
}

SignalService &SignalService::current() {
    FIBER_ASSERT(current_ != nullptr);
    return *current_;
}

SignalService *SignalService::current_or_null() noexcept {
    return current_;
}

bool SignalService::valid_signum(int signum) noexcept {
    return signum > 0 && signum < NSIG;
}

void SignalService::enqueue_waiter(int signum, fiber::async::detail::SignalWaiter *waiter) {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(attached_.load(std::memory_order_acquire));
    FIBER_ASSERT(waiter != nullptr);
    FIBER_ASSERT(valid_signum(signum));
    FIBER_ASSERT(waiter->signum == signum);
    FIBER_ASSERT(waiter->loop == &loop_);
    FIBER_ASSERT(!waiter->queued);

    auto &queue = waiters_[signum];
    waiter->prev = queue.tail;
    waiter->next = nullptr;
    if (queue.tail) {
        queue.tail->next = waiter;
    } else {
        queue.head = waiter;
    }
    queue.tail = waiter;
    waiter->queued = true;
}

void SignalService::cancel_waiter(fiber::async::detail::SignalWaiter *waiter) {
    if (!waiter) {
        return;
    }
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(valid_signum(waiter->signum));
    auto state = waiter->state.load(std::memory_order_acquire);
    if (state == fiber::async::detail::SignalWaiterState::Waiting) {
        if (waiter->queued) {
            if (waiter->prev) {
                waiter->prev->next = waiter->next;
            } else {
                waiters_[waiter->signum].head = waiter->next;
            }
            if (waiter->next) {
                waiter->next->prev = waiter->prev;
            } else {
                waiters_[waiter->signum].tail = waiter->prev;
            }
            waiter->prev = nullptr;
            waiter->next = nullptr;
            waiter->queued = false;
        }
        waiter->state.store(fiber::async::detail::SignalWaiterState::Canceled, std::memory_order_release);
        waiter->handle = {};
        delete waiter;
        return;
    }
    if (state == fiber::async::detail::SignalWaiterState::Notified) {
        waiter->state.store(fiber::async::detail::SignalWaiterState::Canceled, std::memory_order_release);
        waiter->handle = {};
    }
}

bool SignalService::try_pop_pending(int signum, fiber::async::SignalInfo &out) {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(attached_.load(std::memory_order_acquire));
    if (!valid_signum(signum)) {
        return false;
    }
    auto &queue = pending_[signum];
    if (queue.empty()) {
        return false;
    }
    out = queue.front();
    queue.pop_front();
    return true;
}

fiber::async::detail::SignalWaiter *SignalService::pop_next_waiter(int signum) {
    auto &queue = waiters_[signum];
    while (queue.head) {
        auto *waiter = queue.head;
        queue.head = waiter->next;
        if (queue.head) {
            queue.head->prev = nullptr;
        } else {
            queue.tail = nullptr;
        }
        waiter->prev = nullptr;
        waiter->next = nullptr;
        waiter->queued = false;
        auto state = waiter->state.load(std::memory_order_acquire);
        if (state != fiber::async::detail::SignalWaiterState::Waiting) {
            continue;
        }
        waiter->state.store(fiber::async::detail::SignalWaiterState::Notified, std::memory_order_release);
        return waiter;
    }
    return nullptr;
}

void SignalService::on_delivery(const fiber::async::SignalInfo &info) {
    if (!attached_.load(std::memory_order_acquire)) {
        return;
    }
    if (!valid_signum(info.signum)) {
        return;
    }
    auto *waiter = pop_next_waiter(info.signum);
    if (!waiter) {
        pending_[info.signum].push_back(info);
        return;
    }
    waiter->info = info;
    loop_.post<fiber::async::detail::SignalWaiter,
               &fiber::async::detail::SignalWaiter::defer_entry,
               &fiber::async::detail::SignalWaiter::on_run,
               &fiber::async::detail::SignalWaiter::on_cancel>(*waiter);
}

void SignalService::Delivery::on_run(Delivery *self) {
    if (!self) {
        return;
    }
    if (self->service) {
        self->service->on_delivery(self->info);
    }
    delete self;
}

void SignalService::Delivery::on_cancel(Delivery *self) {
    delete self;
}

void SignalService::run_dispatcher(std::stop_token stop_token) {
    pthread_sigmask(SIG_BLOCK, &mask_.native(), nullptr);
    sigset_t wait_mask = mask_.native();

    while (!stop_token.stop_requested()) {
        siginfo_t info{};
        timespec timeout{};
        timeout.tv_sec = 0;
        timeout.tv_nsec = 100 * 1000 * 1000;
        int rc = sigtimedwait(&wait_mask, &info, &timeout);
        if (stop_token.stop_requested()) {
            break;
        }
        if (rc < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            if (errno == EINVAL) {
                break;
            }
            continue;
        }
        if (!attached_.load(std::memory_order_acquire)) {
            continue;
        }
        auto *delivery = new Delivery{};
        delivery->service = this;
        delivery->info = to_signal_info(info);
        loop_.post<Delivery, &Delivery::entry, &Delivery::on_run, &Delivery::on_cancel>(*delivery);
    }
}

} // namespace fiber::event
