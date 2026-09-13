#ifndef FIBER_NET_DETAIL_EFD_H
#define FIBER_NET_DETAIL_EFD_H

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"

namespace fiber::net::detail {

class Efd : public common::NonCopyable, public common::NonMovable {
public:
    using EventCallback = void (*)(void *owner, fiber::event::IoEvent events);

    Efd(fiber::event::EventLoop &loop, void *owner, EventCallback callback,
        fiber::event::Poller::Mode mode = fiber::event::Poller::Mode::None) noexcept;
    ~Efd();

    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept { return loop_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool registered() const noexcept { return registered_; }
    [[nodiscard]] fiber::event::IoEvent watching() const noexcept { return watching_; }
    [[nodiscard]] fiber::event::Poller::Mode mode() const noexcept { return mode_; }

    // Bumped every time the fd is added to or removed from the poller. Callers
    // that re-enter user code compare it to detect a close/re-register in between.
    [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }

    void set_owner(void *owner, EventCallback callback) noexcept;

    fiber::common::IoErr attach(int fd) noexcept;
    int release_fd() noexcept;
    void close_fd() noexcept;

    // Fully remove this fd from poller and clear current interest.
    fiber::common::IoErr unwatch_all() noexcept;

    // Replace the current interest mask. Setting to None removes the fd from poller.
    fiber::common::IoErr watch_set(fiber::event::IoEvent desired) noexcept;
    fiber::common::IoErr watch_add(fiber::event::IoEvent events) noexcept;
    fiber::common::IoErr watch_del(fiber::event::IoEvent events) noexcept;


private:
    struct Item : fiber::event::Poller::Item {
        Efd *efd = nullptr;
    };

    static void on_poller_event(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events);

    fiber::event::EventLoop &loop_;
    void *owner_ = nullptr;
    EventCallback callback_ = nullptr;
    Item item_{};
    int fd_ = -1;
    fiber::event::IoEvent watching_ = fiber::event::IoEvent::None;
    bool registered_ = false;
    std::uint32_t epoch_ = 0;
    fiber::event::Poller::Mode mode_ = fiber::event::Poller::Mode::None;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_EFD_H
