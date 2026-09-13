#ifndef FIBER_EVENT_POLLER_H
#define FIBER_EVENT_POLLER_H

#include <chrono>
#include <cstdint>
#include <sys/epoll.h>
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

    struct Item : common::NonCopyable, common::NonMovable {
        using Callback = void (*)(Item *, int fd, Event);
        Callback callback{};
        int fd() const noexcept { return fd_; }
        std::uint64_t token() const noexcept { return token_; }
        friend class Poller;

    private:
        int fd_{};
        std::uint64_t token_ = 0;
        Event interested_{Event::None};
        friend class EventLoop;
    };

    Poller();
    ~Poller();

    Poller(const Poller &) = delete;
    Poller &operator=(const Poller &) = delete;
    Poller(Poller &&) = delete;
    Poller &operator=(Poller &&) = delete;

    bool valid() const;

    fiber::common::IoErr add(int fd, Event events, Item *item, Mode mode = Mode::None);
    fiber::common::IoErr mod(int fd, Event events, Item *item, Mode mode = Mode::None);
    fiber::common::IoErr del(Item &item);
    [[nodiscard]] Item *resolve(std::uint64_t token) const noexcept;
    int wait(epoll_event *events, int max_events, std::chrono::steady_clock::time_point deadline);

private:
    enum class WaitBackend : std::uint8_t { Unknown, EpollPwait2, TimerFd };

    struct Slot {
        Item *item = nullptr;
        std::uint32_t generation = 1;
        std::uint32_t next = 0;
    };
    static constexpr std::uint32_t kNoSlot = UINT32_MAX;
    static constexpr std::uint64_t kTimerToken = 0;
    bool grow_slots() noexcept;
    void retire_slot(std::uint32_t index) noexcept;
    Slot *slots_ = nullptr;
    std::uint32_t slot_count_ = 0;
    std::uint32_t free_slot_ = kNoSlot;

    int init_timer_fd();
    int wait_timer_fd(epoll_event *events, int max_events, std::chrono::steady_clock::time_point deadline);
    int wait_epoll(epoll_event *events, int max_events, int timeout_ms);
    int sync_timer_fd(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now);
    int drain_timer_fd();

    int epoll_fd_ = -1;
    int timer_fd_ = -1;
    WaitBackend wait_backend_ = WaitBackend::Unknown;
    std::chrono::steady_clock::time_point armed_deadline_ = std::chrono::steady_clock::time_point::max();
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
