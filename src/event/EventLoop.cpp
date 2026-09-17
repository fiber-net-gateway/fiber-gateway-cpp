#include <fiber/event/EventLoop.h>

#include <cerrno>
#include <cstddef>
#include <sys/eventfd.h>
#include <unistd.h>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoopGroup.h>

namespace fiber::event {

thread_local EventLoop *EventLoop::current_ = nullptr;

EventLoop::NotifyEntry::NotifyEntry() : node(this) {}

EventLoop::EventLoop(EventLoopGroup *group, std::size_t group_index) : group_(group), group_index_(group_index) {
    wakeup_entry_.loop = this;
    wakeup_entry_.callback = &EventLoop::on_wakeup;
    // Only the fd is created here. The poller registration happens in
    // run_once(), on the loop's own thread: an EventLoop object is often
    // constructed on one thread and run on another, and every poller
    // operation — including this bootstrap ADD — belongs to the running
    // thread. A wakeup written before the first turn is still delivered:
    // the interest is level-triggered.
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

EventLoop::~EventLoop() {
    if (event_fd_ >= 0) {
        ::close(event_fd_);
    }
}

void EventLoop::notify_wakeup() {
    if (event_fd_ < 0) {
        return;
    }
    if (!wakeup_pending_.exchange(true, std::memory_order_acq_rel)) {
        std::uint64_t one = 1;
        ssize_t written = ::write(event_fd_, &one, sizeof(one));
        (void) written;
    }
}

void EventLoop::enqueue_notify(NotifyNode *node) {
    notify_queue_.push(node);
    notify_wakeup();
}

void EventLoop::on_wakeup(Poller::Item *item, int fd, IoEvent events) {
    (void) fd;
    (void) events;
    auto *entry = static_cast<WakeupEntry *>(item);
    if (!entry || !entry->loop) {
        return;
    }
    entry->loop->drain_wakeup();
}

void EventLoop::drain_wakeup() {
    if (event_fd_ < 0) {
        return;
    }
    std::uint64_t value = 0;
    for (;;) {
        ssize_t rc = ::read(event_fd_, &value, sizeof(value));
        if (rc == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    wakeup_pending_.store(false, std::memory_order_release);
}

void EventLoop::run_due_timers(std::chrono::steady_clock::time_point now) {
    for (;;) {
        TimerEntry *entry = timers_.min();
        if (!entry || entry->deadline > now) {
            break;
        }
        timers_.remove(*entry);
        entry->in_heap_ = false;
        if (entry->callback) {
            entry->callback(entry);
        }
    }
}

std::chrono::steady_clock::time_point EventLoop::next_deadline() const {
    const TimerEntry *entry = timers_.min();
    if (!entry) {
        return std::chrono::steady_clock::time_point::max();
    }
    return entry->deadline;
}

void EventLoop::prepare_run() noexcept { stop_requested_.store(false, std::memory_order_release); }

void EventLoop::run() {
    prepare_run();
    run_prepared();
}

void EventLoop::run_prepared() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    running_.store(true, std::memory_order_release);
    EventLoop *prev = current_;
    current_ = this;
    // The running thread owns the poller from here on; the object itself may
    // have been constructed on a different thread.
    poller_.rebind_owner_thread();
    now_ = std::chrono::steady_clock::now();
    do {
        run_once();
    } while (!stop_requested_.load(std::memory_order_acquire) && event_fd_ >= 0);
    // fd ownership contract: fd wrappers close themselves before the loop
    // stops. Nothing but this loop's wakeup entry may remain (absent when
    // its bootstrap registration failed and closed the fd).
    FIBER_ASSERT_MSG(poller_.size() == (event_fd_ >= 0 ? std::size_t{1} : std::size_t{0}),
                     "poller registrations outlive loop stop");
    current_ = prev;
    running_.store(false, std::memory_order_release);
}

void EventLoop::run_once() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    if (!wakeup_entry_.registered() &&
        poller_.add(event_fd_, IoEvent::Read, &wakeup_entry_) != fiber::common::IoErr::None) {
        // Without the wakeup registration cross-thread posts could never be
        // drained; the loop cannot run. run_prepared() stops on the invalid fd.
        ::close(event_fd_);
        event_fd_ = -1;
        return;
    }
    now_ = std::chrono::steady_clock::now();
    run_due_timers(now_);

    drain_notify();
    drain_defer();

    // Continuations parked with post_next run after this poll: move them behind
    // the (now empty) local queue and poll without blocking so new kernel
    // events, timers and stop interleave with them.
    const bool has_next = !next_queue_.empty();
    local_queue_.splice_back(next_queue_);
    if (!pending_notify_) {
        pending_notify_ = notify_queue_.try_pop_all();
    }
    const bool runnable = has_next || pending_notify_ || stop_requested_.load(std::memory_order_acquire);
    const auto deadline = runnable ? now_ : next_deadline();
    int count = poller_.wait(deadline);
    now_ = std::chrono::steady_clock::now();
    ++turn_;
    if (count < 0) {
        if (errno == EINTR) {
            return;
        }
        return;
    }

    poller_.dispatch();
    drain_defer();
}

void EventLoop::stop() {
    stop_requested_.store(true, std::memory_order_release);
    notify_wakeup();
}

void EventLoop::post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry) {
    FIBER_ASSERT(in_loop());
    FIBER_ASSERT(!entry.in_heap_);
    FIBER_ASSERT(entry.callback != nullptr);

    entry.deadline = when;
    timers_.insert(entry);
    entry.in_heap_ = true;
}

void EventLoop::cancel(TimerEntry &entry) {
    FIBER_ASSERT(in_loop());
    if (!entry.in_heap_) {
        return;
    }
    timers_.remove(entry);
    entry.in_heap_ = false;
}

void EventLoop::cancel_quiesced(TimerEntry &entry) {
    FIBER_ASSERT(!running());
    if (!entry.in_heap_) {
        return;
    }
    timers_.remove(entry);
    entry.in_heap_ = false;
}

void EventLoop::cancel(DeferEntry &entry) {
    FIBER_ASSERT(in_loop());
    // The entry may sit in either the local or the next queue; the ring hook
    // unlinks without knowing which.
    entry.node_.unlink_self();
}

} // namespace fiber::event
