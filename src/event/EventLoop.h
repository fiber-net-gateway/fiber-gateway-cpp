#ifndef FIBER_EVENT_EVENT_LOOP_H
#define FIBER_EVENT_EVENT_LOOP_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>

#include "../async/Scheduler.h"
#include "MpscQueue.h"
#include "Poller.h"
#include "TimerQueue.h"

namespace fiber::event {

using IoEvent = Poller::Event;

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

        bool operator<(const TimerEntry &other) const noexcept {
            if (deadline != other.deadline) {
                return deadline < other.deadline;
            }
            return id < other.id;
        }
    };

    struct WatchEntry : Poller::Item {
        int watched_fd = -1;
        IoEvent events = IoEvent::Read;
        IoCallback io_callback{};
        WatchReady on_ready{};
        std::uint64_t id = 0;
        bool registered = false;
    };

    struct WakeupEntry : Poller::Item {
        EventLoop *loop = nullptr;
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

    static TimerEntry *timer_from_node(TimerQueue::Node *node) noexcept;
    friend bool operator<(const TimerQueue::Node &a, const TimerQueue::Node &b) noexcept;

    static void on_wakeup(Poller::Item *item, int fd, IoEvent events);
    static void on_watch_event(Poller::Item *item, int fd, IoEvent events);

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
    Poller poller_;
    int event_fd_ = -1;
    WakeupEntry wakeup_entry_{};
    std::atomic<bool> wakeup_pending_{false};
    std::atomic<bool> stop_requested_{false};
};

} // namespace fiber::event

#endif // FIBER_EVENT_EVENT_LOOP_H
