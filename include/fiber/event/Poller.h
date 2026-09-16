#ifndef FIBER_EVENT_POLLER_H
#define FIBER_EVENT_POLLER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sys/epoll.h>
#include <thread>
#include <type_traits>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::event {

class Poller {
public:
    enum class Event : std::uint32_t {
        None = 0,
        Read = 1u << 0,
        Write = 1u << 1,
        // EPOLLERR/EPOLLHUP only. Deliberately excludes EPOLLRDHUP so a peer
        // that half-closes its write side can continue receiving data.
        Terminal = 1u << 2,
        ReadHangup = 1u << 3,
    };
    enum class Mode : std::uint32_t { None = 0, Edge = 1u << 0, OneShot = 1u << 1 };

    // Registered items are addressed directly through epoll_event::data.ptr.
    // An item must stay alive until del() returns; del() also blanks any entry
    // still pointing at it in the batch most recently returned by wait(), so
    // a callback may destroy other items from the same kernel batch.
    struct Item : common::NonCopyable, common::NonMovable {
        using Callback = void (*)(Item *, int fd, Event);
        Callback callback{};
        int fd() const noexcept { return fd_; }
        bool registered() const noexcept { return registered_; }
        friend class Poller;

    private:
        int fd_{};
        Event interested_{Event::None};
        bool registered_ = false;
        friend class EventLoop;
    };

    Poller();
    ~Poller();

    Poller(const Poller &) = delete;
    Poller &operator=(const Poller &) = delete;
    Poller(Poller &&) = delete;
    Poller &operator=(Poller &&) = delete;

    bool valid() const;

    // Number of registered items; loop-thread only. The loop asserts this is
    // down to its own wakeup entry before it stops: fd wrappers must have
    // closed their registrations by then.
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // All poller operations belong to one thread: the constructing thread,
    // re-anchored by rebind_owner_thread() when an EventLoop hands its poller
    // to the thread that runs the loop (EventLoop objects are constructed on
    // one thread and run on another). Debug builds assert the affinity so a
    // cross-thread add/mod/del/wait fails loudly instead of racing the batch.
    void rebind_owner_thread() noexcept { owner_thread_ = std::this_thread::get_id(); }

    fiber::common::IoErr add(int fd, Event events, Item *item, Mode mode = Mode::None);
    fiber::common::IoErr mod(int fd, Event events, Item *item, Mode mode = Mode::None);
    fiber::common::IoErr del(Item &item);
    // Returns ready events with data.ptr set to the registered Item (nullptr
    // entries must be skipped). The array stays the "current batch" until
    // end_batch() is called or the next wait() begins.
    int wait(epoll_event *events, int max_events, std::chrono::steady_clock::time_point deadline);
    void end_batch() noexcept {
        batch_ = nullptr;
        batch_count_ = 0;
    }

private:
    enum class WaitBackend : std::uint8_t { Unknown, EpollPwait2, TimerFd };

    void assert_owner_thread() const noexcept;
    void invalidate_batch(const Item &item) noexcept;

    int wait_impl(epoll_event *events, int max_events, std::chrono::steady_clock::time_point deadline);
    int init_timer_fd();
    int wait_timer_fd(epoll_event *events, int max_events, std::chrono::steady_clock::time_point deadline);
    int wait_epoll(epoll_event *events, int max_events, int timeout_ms);
    int sync_timer_fd(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now);
    int drain_timer_fd();

    int epoll_fd_ = -1;
    int timer_fd_ = -1;
    std::size_t size_ = 0;
    epoll_event *batch_ = nullptr;
    int batch_count_ = 0;
    WaitBackend wait_backend_ = WaitBackend::Unknown;
    std::chrono::steady_clock::time_point armed_deadline_ = std::chrono::steady_clock::time_point::max();
    std::thread::id owner_thread_{};
};

constexpr Poller::Event operator|(Poller::Event left, Poller::Event right) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(static_cast<U>(left) | static_cast<U>(right));
}

constexpr Poller::Event operator&(Poller::Event left, Poller::Event right) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(static_cast<U>(left) & static_cast<U>(right));
}

constexpr Poller::Event operator^(Poller::Event left, Poller::Event right) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(static_cast<U>(left) ^ static_cast<U>(right));
}

constexpr Poller::Event operator~(Poller::Event value) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(~static_cast<U>(value));
}

constexpr Poller::Mode operator|(Poller::Mode left, Poller::Mode right) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(static_cast<U>(left) | static_cast<U>(right));
}

constexpr Poller::Mode operator&(Poller::Mode left, Poller::Mode right) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(static_cast<U>(left) & static_cast<U>(right));
}

constexpr Poller::Mode operator^(Poller::Mode left, Poller::Mode right) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(static_cast<U>(left) ^ static_cast<U>(right));
}

constexpr Poller::Mode operator~(Poller::Mode value) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(~static_cast<U>(value));
}

inline Poller::Event &operator|=(Poller::Event &left, Poller::Event right) noexcept {
    left = left | right;
    return left;
}

inline Poller::Event &operator&=(Poller::Event &left, Poller::Event right) noexcept {
    left = left & right;
    return left;
}

inline Poller::Event &operator^=(Poller::Event &left, Poller::Event right) noexcept {
    left = left ^ right;
    return left;
}

inline Poller::Mode &operator|=(Poller::Mode &left, Poller::Mode right) noexcept {
    left = left | right;
    return left;
}

inline Poller::Mode &operator&=(Poller::Mode &left, Poller::Mode right) noexcept {
    left = left & right;
    return left;
}

inline Poller::Mode &operator^=(Poller::Mode &left, Poller::Mode right) noexcept {
    left = left ^ right;
    return left;
}

constexpr bool any(Poller::Event events) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<U>(events) != 0;
}

} // namespace fiber::event

#endif // FIBER_EVENT_POLLER_H
