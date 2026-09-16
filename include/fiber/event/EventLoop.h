#ifndef FIBER_EVENT_EVENT_LOOP_H
#define FIBER_EVENT_EVENT_LOOP_H

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../common/BinaryHeap.h"
#include "../common/IntrusiveList.h"
#include "../common/mem/IoBufChain.h"
#include "MpscQueue.h"
#include "Poller.h"

namespace fiber::event {

using IoEvent = Poller::Event;

class EventLoopGroup;

namespace detail {

template<typename Handle, typename Entry, auto EntryMember>
concept TimerEntryMember = std::is_object_v<Handle> && !std::is_const_v<Handle> &&
                           std::same_as<decltype(EntryMember), Entry Handle::*> && requires(Handle &handle) {
                               { handle.*EntryMember } -> std::same_as<Entry &>;
                           };

template<typename Handle, typename Entry, auto EntryMember>
concept NotifyEntryMember = std::is_object_v<Handle> && !std::is_const_v<Handle> &&
                            std::same_as<decltype(EntryMember), Entry Handle::*> && requires(Handle &handle) {
                                { handle.*EntryMember } -> std::same_as<Entry &>;
                            };

template<typename Handle, typename Entry, auto EntryMember>
concept DeferEntryMember = std::is_object_v<Handle> && !std::is_const_v<Handle> &&
                           std::same_as<decltype(EntryMember), Entry Handle::*> && requires(Handle &handle) {
                               { handle.*EntryMember } -> std::same_as<Entry &>;
                           };

template<typename Handle, typename Entry, auto EntryMember>
concept StopEntryMember = std::is_object_v<Handle> && !std::is_const_v<Handle> &&
                          std::same_as<decltype(EntryMember), Entry Handle::*> && requires(Handle &handle) {
                              { handle.*EntryMember } -> std::same_as<Entry &>;
                          };

template<typename Handle, auto Cb>
concept TimerCallback = std::same_as<decltype(Cb), void (*)(Handle *) noexcept>;

template<typename Handle, auto Cb>
concept NotifyCallback = std::same_as<decltype(Cb), void (*)(Handle *) noexcept>;

template<typename Handle, auto Cb>
concept DeferCallback = std::same_as<decltype(Cb), void (*)(Handle *) noexcept>;

template<typename Handle, auto Cb>
concept StopCallback = std::same_as<decltype(Cb), void (*)(Handle *) noexcept>;

} // namespace detail

class EventLoop {
public:
    static constexpr std::size_t kInvalidGroupIndex = static_cast<std::size_t>(-1);

    struct NotifyEntry {
        friend class EventLoop;

    public:
        using Callback = void (*)(NotifyEntry *);

        NotifyEntry();
        NotifyEntry(const NotifyEntry &) = delete;
        NotifyEntry &operator=(const NotifyEntry &) = delete;
        NotifyEntry(NotifyEntry &&) = delete;
        NotifyEntry &operator=(NotifyEntry &&) = delete;

    private:
        Callback on_run = nullptr;
        MpscQueue<NotifyEntry *>::Node node;
        std::ptrdiff_t handle_offset = 0;
    };

    struct TimerEntry {
        friend class EventLoop;

    public:
        using Callback = void (*)(TimerEntry *);

        TimerEntry() = default;
        TimerEntry(const TimerEntry &) = delete;
        TimerEntry &operator=(const TimerEntry &) = delete;
        TimerEntry(TimerEntry &&) = delete;
        TimerEntry &operator=(TimerEntry &&) = delete;

        bool operator<(const TimerEntry &other) const noexcept { return deadline < other.deadline; }
        [[nodiscard]] bool is_in_heap() const noexcept { return in_heap_; }

    private:
        Callback callback = nullptr;
        std::chrono::steady_clock::time_point deadline{};
        common::BinaryHeapNode node{};
        bool in_heap_ = false;
        std::ptrdiff_t handle_offset = 0;
    };

    struct TimerEntryCompare {
        bool operator()(const TimerEntry *a, const TimerEntry *b) const noexcept { return *a < *b; }
    };

    struct DeferEntry {
    public:
        friend class EventLoop;

        using Callback = void (*)(DeferEntry *);
        // Queued state lives in the hook: an unlinked hook is self-linked, so
        // no separate flag is needed and a queued entry unlinks itself if it
        // is destroyed.
        [[nodiscard]] bool is_in_queue() const noexcept { return node_.linked(); }

    private:
        common::IntrusiveListHook node_{};
        Callback callback_ = nullptr;
        std::ptrdiff_t handle_offset_ = 0;
    };

    // Intrusive, loop-thread-only callback used by pending operations that must be canceled
    // before run() returns. The loop removes an entry before invoking it, so the callback may
    // resume a coroutine that destroys the entry's owner.
    struct StopEntry {
    public:
        friend class EventLoop;

        using Callback = void (*)(StopEntry *) noexcept;
        [[nodiscard]] bool is_registered() const noexcept { return node_.linked(); }

    private:
        common::IntrusiveListHook node_{};
        Callback callback_ = nullptr;
        std::ptrdiff_t handle_offset_ = 0;
    };

    explicit EventLoop(EventLoopGroup *group = nullptr, std::size_t group_index = kInvalidGroupIndex);
    ~EventLoop();

    void run();
    void run_once();
    void stop();

    [[nodiscard]] static EventLoop &current() noexcept {
        FIBER_ASSERT(current_ != nullptr);
        return *current_;
    }
    [[nodiscard]] static EventLoop *current_or_null() noexcept { return current_; }

    [[nodiscard]] bool in_loop() const noexcept { return current_or_null() == this; }
    [[nodiscard]] bool valid() const noexcept { return event_fd_ >= 0 && poller_.valid(); }
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    // True once stop() has been requested for the current run (any thread); the
    // next run() resets it. Loop-affine adoption paths use this to refuse
    // taking over work on a loop that is going away.
    [[nodiscard]] bool stopping() const noexcept { return stop_requested_.load(std::memory_order_acquire); }
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept { return now_; }
    // Incremented by every poll. Loop-thread only; lets budgeted consumers
    // account work per turn and yield with post_next once their share is spent.
    [[nodiscard]] std::uint64_t turn() const noexcept { return turn_; }


    template<typename Handle, auto EntryMember, auto RunCb>
        requires detail::NotifyEntryMember<Handle, NotifyEntry, EntryMember> && detail::NotifyCallback<Handle, RunCb>
    void post(Handle &handle) noexcept {
        NotifyEntry &entry = handle.*EntryMember;
        entry.handle_offset = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.on_run = &EventLoop::notify_trampoline<Handle, EntryMember, RunCb>;
        enqueue_notify(&entry.node);
    }

    // Runs in the current turn. The loop drains this queue to empty before it
    // polls, so work posted here (including work posted by the callbacks it
    // runs) never waits for the kernel. A callback that keeps re-posting itself
    // here therefore never lets the loop poll; continuations that yield on
    // purpose must use post_next instead.
    template<typename Handle, auto EntryMember, auto RunCb>
        requires detail::DeferEntryMember<Handle, DeferEntry, EntryMember> && detail::DeferCallback<Handle, RunCb>
    void post_local(Handle &handle) noexcept {
        enqueue_defer<Handle, EntryMember, RunCb>(handle, local_queue_);
    }

    // Runs after the next poll (nginx posted_next_events semantics). A
    // non-empty next queue makes that poll non-blocking; its entries are moved
    // to the local queue right before the poll, so event callbacks run first
    // and local work they post runs behind the moved entries. Kernel events,
    // timers and stop therefore always interleave between two runs of the same
    // continuation. An entry that is already queued keeps its position.
    template<typename Handle, auto EntryMember, auto RunCb>
        requires detail::DeferEntryMember<Handle, DeferEntry, EntryMember> && detail::DeferCallback<Handle, RunCb>
    void post_next(Handle &handle) noexcept {
        enqueue_defer<Handle, EntryMember, RunCb>(handle, next_queue_);
    }

    template<typename Handle, auto EntryMember>
        requires detail::DeferEntryMember<Handle, DeferEntry, EntryMember>
    void cancel(Handle &handle) {
        DeferEntry &entry = handle.*EntryMember;
        cancel(entry);
    }

    template<typename Handle, auto EntryMember, auto Cb>
        requires detail::TimerEntryMember<Handle, TimerEntry, EntryMember> && detail::TimerCallback<Handle, Cb>
    void post_at(std::chrono::steady_clock::time_point when, Handle &handle) {
        TimerEntry &entry = handle.*EntryMember;
        entry.handle_offset = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.callback = &EventLoop::timer_trampoline<Handle, EntryMember, Cb>;
        post_at(when, entry);
    }

    template<typename Handle, auto EntryMember>
        requires detail::TimerEntryMember<Handle, TimerEntry, EntryMember>
    void cancel(Handle &handle) {
        TimerEntry &entry = handle.*EntryMember;
        cancel(entry);
    }

    template<typename Handle, auto EntryMember>
        requires detail::TimerEntryMember<Handle, TimerEntry, EntryMember>
    void cancel_quiesced(Handle &handle) {
        TimerEntry &entry = handle.*EntryMember;
        cancel_quiesced(entry);
    }

    template<typename Handle, auto EntryMember, auto Cb>
        requires detail::StopEntryMember<Handle, StopEntry, EntryMember> && detail::StopCallback<Handle, Cb>
    [[nodiscard]] bool register_stop(Handle &handle) noexcept {
        FIBER_ASSERT(in_loop());
        StopEntry &entry = handle.*EntryMember;
        FIBER_ASSERT(!entry.node_.linked());
        if (stopping()) {
            return false;
        }
        entry.handle_offset_ = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.callback_ = &EventLoop::stop_trampoline<Handle, EntryMember, Cb>;
        stop_queue_.push_back(entry);
        return true;
    }

    template<typename Handle, auto EntryMember>
        requires detail::StopEntryMember<Handle, StopEntry, EntryMember>
    void unregister_stop(Handle &handle) noexcept {
        FIBER_ASSERT(in_loop());
        stop_queue_.erase(handle.*EntryMember);
    }

    Poller &poller() noexcept { return poller_; }
    const Poller &poller() const noexcept { return poller_; }
    mem::IoBufNodePool &io_buf_node_pool() noexcept { return io_buf_node_pool_; }

    EventLoopGroup *group() noexcept { return group_; }

    const EventLoopGroup *group() const noexcept { return group_; }

    [[nodiscard]] bool has_group_index() const noexcept { return group_index_ != kInvalidGroupIndex; }

    [[nodiscard]] std::size_t group_index() const noexcept {
        FIBER_ASSERT(group_index_ != kInvalidGroupIndex);
        return group_index_;
    }

private:
    friend class EventLoopGroup;

    using DeferQueue = common::IntrusiveList<DeferEntry, offsetof(DeferEntry, node_)>;
    using StopQueue = common::IntrusiveList<StopEntry, offsetof(StopEntry, node_)>;

    static thread_local EventLoop *current_;

    struct WakeupEntry : Poller::Item {
        EventLoop *loop = nullptr;
    };
    using NotifyNode = MpscQueue<NotifyEntry *>::Node;

    static void on_wakeup(Poller::Item *item, int fd, IoEvent events);

    void prepare_run() noexcept;
    void run_prepared();

    template<typename Handle, auto EntryMember, auto Cb>
    static void notify_trampoline(NotifyEntry *entry) {
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset);
        Cb(handle);
    }

    template<typename Handle, auto EntryMember, auto Cb>
    static void timer_trampoline(TimerEntry *entry) {
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset);
        Cb(handle);
    }

    template<typename Handle, auto EntryMember, auto Cb>
    static void defer_trampoline(DeferEntry *entry) {
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset_);
        Cb(handle);
    }

    template<typename Handle, auto EntryMember, auto Cb>
    static void stop_trampoline(StopEntry *entry) noexcept {
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset_);
        Cb(handle);
    }

    void notify_wakeup();
    void enqueue_notify(NotifyNode *node);
    template<typename Handle, auto EntryMember, auto RunCb>
    void enqueue_defer(Handle &handle, DeferQueue &queue) noexcept {
        DeferEntry &entry = handle.*EntryMember;
        entry.handle_offset_ = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.callback_ = &EventLoop::defer_trampoline<Handle, EntryMember, RunCb>;
        // push_back keeps an already queued entry where it is.
        queue.push_back(entry);
    }

    // Runs one batch of cross-thread notifications. Entries that arrive while
    // the batch runs are picked up by the next turn, which polls without
    // blocking.
    void drain_notify() {
        if (!pending_notify_) {
            pending_notify_ = notify_queue_.try_pop_all();
        }
        while (pending_notify_) {
            NotifyNode *node = pending_notify_;
            pending_notify_ = MpscQueue<NotifyEntry *>::next(node);
            NotifyEntry *entry = MpscQueue<NotifyEntry *>::unwrap(node);
            MpscQueue<NotifyEntry *>::reset(node);
            entry->on_run(entry);
        }
    }

    // Runs local work to empty, including entries posted while draining.
    // Entries stay in the loop queue until they run so cancellation, including
    // destruction from another callback, keeps working.
    void drain_defer() {
        while (DeferEntry *entry = local_queue_.front()) {
            local_queue_.erase(*entry);
            entry->callback_(entry);
        }
    }

    void drain_wakeup();
    void drain_stop() noexcept;
    void run_due_timers(std::chrono::steady_clock::time_point now);
    std::chrono::steady_clock::time_point next_deadline() const;
    void post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry);
    void cancel(TimerEntry &entry);
    void cancel_quiesced(TimerEntry &entry);
    void cancel(DeferEntry &entry);

    MpscQueue<NotifyEntry *> notify_queue_;
    NotifyNode *pending_notify_ = nullptr;
    // Loop-thread only: timer heap operations.
    DeferQueue local_queue_;
    DeferQueue next_queue_;
    std::uint64_t turn_ = 0;
    StopQueue stop_queue_;
    common::BinaryHeap<TimerEntry, offsetof(TimerEntry, node), TimerEntryCompare> timers_;
    Poller poller_;
    int event_fd_ = -1;
    WakeupEntry wakeup_entry_{};
    std::atomic<bool> wakeup_pending_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point now_{};
    mem::IoBufNodePool io_buf_node_pool_{};
    EventLoopGroup *group_ = nullptr;
    std::size_t group_index_ = kInvalidGroupIndex;
};

} // namespace fiber::event

#endif // FIBER_EVENT_EVENT_LOOP_H
