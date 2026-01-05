#include "EventLoop.h"

#include <cerrno>
#include <cstddef>
#include <limits>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace fiber::event {

namespace {

constexpr std::uint8_t to_mask(IoEvent events) {
    return static_cast<std::uint8_t>(events);
}

std::uint32_t to_epoll_events(IoEvent events) {
    std::uint32_t mask = 0;
    auto bits = to_mask(events);
    if (bits & to_mask(IoEvent::Read)) {
        mask |= EPOLLIN;
    }
    if (bits & to_mask(IoEvent::Write)) {
        mask |= EPOLLOUT;
    }
    mask |= EPOLLERR | EPOLLHUP;
    return mask;
}

IoEvent to_io_event(std::uint32_t events) {
    std::uint8_t mask = 0;
    if (events & (EPOLLIN | EPOLLPRI)) {
        mask |= to_mask(IoEvent::Read);
    }
    if (events & EPOLLOUT) {
        mask |= to_mask(IoEvent::Write);
    }
    if (events & (EPOLLERR | EPOLLHUP)) {
        mask |= to_mask(IoEvent::Error);
    }
    return static_cast<IoEvent>(mask);
}

} // namespace

EventLoop::EventLoop() {
    timers_.init();
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        return;
    }
    if (!poller_.valid()) {
        ::close(event_fd_);
        event_fd_ = -1;
        return;
    }
    if (poller_.add(event_fd_, EPOLLIN, this) != 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

EventLoop::~EventLoop() {
    if (event_fd_ >= 0) {
        ::close(event_fd_);
    }
}

bool EventLoop::timer_less(const TimerQueue::Node *a, const TimerQueue::Node *b) noexcept {
    const auto *left = timer_from_node(const_cast<TimerQueue::Node *>(a));
    const auto *right = timer_from_node(const_cast<TimerQueue::Node *>(b));
    if (left->deadline != right->deadline) {
        return left->deadline < right->deadline;
    }
    return left->id < right->id;
}

EventLoop::TimerEntry *EventLoop::timer_from_node(TimerQueue::Node *node) noexcept {
    if (!node) {
        return nullptr;
    }
    return reinterpret_cast<TimerEntry *>(reinterpret_cast<char *>(node) - offsetof(TimerEntry, node));
}

void EventLoop::enqueue(CommandNode *node) {
    command_queue_.push(node);
    if (event_fd_ < 0) {
        return;
    }
    if (!wakeup_pending_.exchange(true, std::memory_order_acq_rel)) {
        std::uint64_t one = 1;
        ssize_t written = ::write(event_fd_, &one, sizeof(one));
        (void)written;
    }
}

void EventLoop::drain_commands() {
    CommandNode *node = command_queue_.try_pop_all();
    while (node) {
        CommandNode *next = MpscQueue<Command>::next(node);
        Command &cmd = MpscQueue<Command>::unwrap(node);
        switch (cmd.type) {
        case CommandType::Task:
            if (cmd.task) {
                cmd.task();
            }
            break;
        case CommandType::Resume:
            if (cmd.handle) {
                cmd.handle.resume();
            }
            break;
        case CommandType::AddTimer:
            if (cmd.timer) {
                if (cmd.timer->cancelled) {
                    delete cmd.timer;
                } else if (!cmd.timer->in_heap) {
                    timers_.insert(&cmd.timer->node, &EventLoop::timer_less);
                    cmd.timer->in_heap = true;
                }
            }
            break;
        case CommandType::CancelTimer:
            if (cmd.timer) {
                cmd.timer->cancelled = true;
                if (cmd.timer->in_heap) {
                    timers_.remove(&cmd.timer->node, &EventLoop::timer_less);
                    cmd.timer->in_heap = false;
                }
                delete cmd.timer;
            }
            break;
        case CommandType::WatchFd:
            if (cmd.watch) {
                int rc = poller_.add(cmd.watch->fd, to_epoll_events(cmd.watch->events), cmd.watch);
                cmd.watch->registered = (rc == 0);
                if (cmd.watch->on_ready) {
                    cmd.watch->on_ready(rc == 0 ? 0 : errno);
                }
            }
            break;
        case CommandType::UpdateFd:
            if (cmd.watch) {
                cmd.watch->events = cmd.events;
                if (cmd.watch->registered) {
                    poller_.mod(cmd.watch->fd, to_epoll_events(cmd.watch->events), cmd.watch);
                }
            }
            break;
        case CommandType::UnwatchFd:
            if (cmd.watch) {
                if (cmd.watch->registered) {
                    poller_.del(cmd.watch->fd);
                    cmd.watch->registered = false;
                }
                delete cmd.watch;
            }
            break;
        case CommandType::Stop:
            stop_requested_.store(true, std::memory_order_release);
            break;
        }
        delete node;
        node = next;
    }
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
        TimerQueue::Node *node = timers_.min();
        if (!node) {
            break;
        }
        TimerEntry *entry = timer_from_node(node);
        if (!entry || entry->deadline > now) {
            break;
        }
        timers_.dequeue(&EventLoop::timer_less);
        entry->in_heap = false;
        if (!entry->cancelled && entry->callback) {
            entry->callback();
        }
        delete entry;
    }
}

int EventLoop::next_timeout_ms(std::chrono::steady_clock::time_point now) const {
    TimerQueue::Node *node = timers_.min();
    if (!node) {
        return -1;
    }
    const TimerEntry *entry = timer_from_node(const_cast<TimerQueue::Node *>(node));
    if (!entry) {
        return -1;
    }
    if (entry->deadline <= now) {
        return 0;
    }
    auto delta = entry->deadline - now;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    if (ms <= 0) {
        return 0;
    }
    if (ms > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ms);
}

void EventLoop::run() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    stop_requested_.store(false, std::memory_order_release);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        run_once();
    }
}

void EventLoop::run_once() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    drain_commands();
    if (stop_requested_.load(std::memory_order_acquire)) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    run_due_timers(now);

    int timeout_ms = next_timeout_ms(now);
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    int count = poller_.wait(events, kMaxEvents, timeout_ms);
    if (count < 0) {
        if (errno == EINTR) {
            return;
        }
        return;
    }

    for (int i = 0; i < count; ++i) {
        void *data = events[i].data.ptr;
        if (data == this) {
            drain_wakeup();
            continue;
        }
        auto *watch = static_cast<WatchEntry *>(data);
        if (!watch || !watch->registered || !watch->callback) {
            continue;
        }
        IoEvent io = to_io_event(events[i].events);
        if (to_mask(io) == 0) {
            continue;
        }
        watch->callback(io);
    }
}

void EventLoop::stop() {
    auto *node = new CommandNode(Command{CommandType::Stop});
    enqueue(node);
}

void EventLoop::post(TaskFn fn) {
    auto *node = new CommandNode(Command{CommandType::Task, std::move(fn)});
    enqueue(node);
}

void EventLoop::post(std::coroutine_handle<> handle) {
    Command cmd{};
    cmd.type = CommandType::Resume;
    cmd.handle = handle;
    auto *node = new CommandNode(std::move(cmd));
    enqueue(node);
}

EventLoop::TimerHandle EventLoop::post_after(std::chrono::steady_clock::duration delay, TaskFn fn) {
    return post_at(std::chrono::steady_clock::now() + delay, std::move(fn));
}

EventLoop::TimerHandle EventLoop::post_at(std::chrono::steady_clock::time_point when, TaskFn fn) {
    auto *entry = new TimerEntry();
    entry->deadline = when;
    entry->callback = std::move(fn);
    entry->id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);

    Command cmd{};
    cmd.type = CommandType::AddTimer;
    cmd.timer = entry;
    auto *node = new CommandNode(std::move(cmd));
    enqueue(node);

    TimerHandle handle{};
    handle.entry_ = entry;
    handle.id_ = entry->id;
    return handle;
}

void EventLoop::cancel(TimerHandle handle) {
    if (!handle.valid()) {
        return;
    }
    Command cmd{};
    cmd.type = CommandType::CancelTimer;
    cmd.timer = handle.entry_;
    auto *node = new CommandNode(std::move(cmd));
    enqueue(node);
}

EventLoop::WatchHandle EventLoop::watch_fd(int fd, IoEvent events, IoCallback cb, WatchReady on_ready) {
    auto *entry = new WatchEntry();
    entry->fd = fd;
    entry->events = events;
    entry->callback = std::move(cb);
    entry->on_ready = std::move(on_ready);
    entry->id = next_watch_id_.fetch_add(1, std::memory_order_relaxed);

    Command cmd{};
    cmd.type = CommandType::WatchFd;
    cmd.watch = entry;
    auto *node = new CommandNode(std::move(cmd));
    enqueue(node);

    WatchHandle handle{};
    handle.entry_ = entry;
    handle.id_ = entry->id;
    return handle;
}

void EventLoop::update_fd(WatchHandle handle, IoEvent events) {
    if (!handle.valid()) {
        return;
    }
    Command cmd{};
    cmd.type = CommandType::UpdateFd;
    cmd.watch = handle.entry_;
    cmd.events = events;
    auto *node = new CommandNode(std::move(cmd));
    enqueue(node);
}

void EventLoop::unwatch_fd(WatchHandle handle) {
    if (!handle.valid()) {
        return;
    }
    Command cmd{};
    cmd.type = CommandType::UnwatchFd;
    cmd.watch = handle.entry_;
    auto *node = new CommandNode(std::move(cmd));
    enqueue(node);
}

} // namespace fiber::event
