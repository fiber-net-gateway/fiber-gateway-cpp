#ifndef FIBER_EVENT_EVENT_LOOP_H
#define FIBER_EVENT_EVENT_LOOP_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>

#include "../async/Scheduler.h"
#include "EpollPoller.h"
#include "MpscQueue.h"
#include "TimerQueue.h"

namespace fiber::event {

enum class IoEvent : std::uint8_t {
    Read = 1 << 0,
    Write = 1 << 1,
    Error = 1 << 2
};

class EventLoop : public fiber::async::IScheduler {
private:
    struct TimerEntry;
    struct WatchEntry;

public:
    using TaskFn = std::function<void()>;
    using IoCallback = std::function<void(IoEvent)>;
    using WatchReady = std::function<void(int error)>;

    struct TimerHandle {
        bool valid() const {
            return entry_ != nullptr && id_ != 0;
        }

    private:
        friend class EventLoop;

        TimerEntry *entry_ = nullptr;
        std::uint64_t id_ = 0;
    };

    struct WatchHandle {
        bool valid() const {
            return entry_ != nullptr && id_ != 0;
        }

    private:
        friend class EventLoop;

        WatchEntry *entry_ = nullptr;
        std::uint64_t id_ = 0;
    };

    EventLoop();
    ~EventLoop() override;

    void run();
    void run_once();
    void stop();

    void post(TaskFn fn);
    void post(std::coroutine_handle<> handle) override;

    TimerHandle post_after(std::chrono::steady_clock::duration delay, TaskFn fn);
    TimerHandle post_at(std::chrono::steady_clock::time_point when, TaskFn fn);
    void cancel(TimerHandle handle);

    WatchHandle watch_fd(int fd, IoEvent events, IoCallback cb, WatchReady on_ready = {});
    void update_fd(WatchHandle handle, IoEvent events);
    void unwatch_fd(WatchHandle handle);

private:
    enum class CommandType : std::uint8_t {
        Task,
        Resume,
        AddTimer,
        CancelTimer,
        WatchFd,
        UpdateFd,
        UnwatchFd,
        Stop
    };

    struct TimerEntry {
        TimerQueue::Node node;
        std::chrono::steady_clock::time_point deadline{};
        TaskFn callback{};
        std::uint64_t id = 0;
        bool cancelled = false;
        bool in_heap = false;
    };

    struct WatchEntry {
        int fd = -1;
        IoEvent events = IoEvent::Read;
        IoCallback callback{};
        WatchReady on_ready{};
        std::uint64_t id = 0;
        bool registered = false;
    };

    struct Command {
        CommandType type = CommandType::Task;
        TaskFn task{};
        std::coroutine_handle<> handle{};
        TimerEntry *timer = nullptr;
        WatchEntry *watch = nullptr;
        IoEvent events = IoEvent::Read;
    };

    using CommandNode = MpscQueue<Command>::Node;

    static bool timer_less(const TimerQueue::Node *a, const TimerQueue::Node *b) noexcept;
    static TimerEntry *timer_from_node(TimerQueue::Node *node) noexcept;

    void enqueue(CommandNode *node);
    void drain_commands();
    void drain_wakeup();
    void run_due_timers(std::chrono::steady_clock::time_point now);
    int next_timeout_ms(std::chrono::steady_clock::time_point now) const;

    MpscQueue<Command> command_queue_;
    // Loop-thread only: timer heap operations.
    TimerQueue timers_;
    std::atomic<std::uint64_t> next_timer_id_{1};
    std::atomic<std::uint64_t> next_watch_id_{1};
    EpollPoller poller_;
    int event_fd_ = -1;
    std::atomic<bool> wakeup_pending_{false};
    std::atomic<bool> stop_requested_{false};
};

} // namespace fiber::event

#endif // FIBER_EVENT_EVENT_LOOP_H
