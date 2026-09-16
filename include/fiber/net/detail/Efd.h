#ifndef FIBER_NET_DETAIL_EFD_H
#define FIBER_NET_DETAIL_EFD_H

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"

namespace fiber::net::detail {

// Raw poller registration for one fd with single-thread ownership.
//
// An Efd has a fixed `owner_loop` (creation-time home, used by upper pools for
// lifecycle coordination) and a mutable `current_loop` pointer that names the
// only loop allowed to operate the fd's poller registration and I/O at a given
// moment. Current starts at owner and only moves through the explicit
// handover pair: detach_from_current_loop() (old loop, DEL + clear interest)
// followed by adopt_loop() (target loop, no re-add; the first listening demand
// re-registers). While detached the object is owned by the handover message.
class Efd : public common::NonCopyable, public common::NonMovable {
public:
    using EventCallback = void (*)(void *sink, fiber::event::IoEvent events);

    Efd(fiber::event::EventLoop &owner_loop, void *sink, EventCallback callback,
        fiber::event::Poller::Mode mode = fiber::event::Poller::Mode::None) noexcept;
    ~Efd();

    [[nodiscard]] fiber::event::EventLoop &owner_loop() const noexcept { return owner_loop_; }
    [[nodiscard]] fiber::event::EventLoop &current_loop() const noexcept { return *current_loop_; }
    // Alias for current_loop(); callers that never hand over may keep using it.
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept { return *current_loop_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool registered() const noexcept { return registered_; }
    [[nodiscard]] fiber::event::IoEvent watching() const noexcept { return watching_; }
    [[nodiscard]] fiber::event::Poller::Mode mode() const noexcept { return mode_; }

    // Bumped every time the fd is added to or removed from the poller. Callers
    // that re-enter user code compare it to detect a close/re-register in between.
    [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }

    void set_event_sink(void *sink, EventCallback callback) noexcept;

    fiber::common::IoErr attach(int fd) noexcept;
    int release_fd() noexcept;
    void close_fd() noexcept;

    // Handover step 1 (old current loop): remove the fd from its poller and
    // clear registration state and interest. The fd itself stays open. On
    // failure the caller must keep the object on the old loop and clean it up
    // there (typically by closing); the fd must not be published.
    fiber::common::IoErr detach_from_current_loop() noexcept;
    // Handover step 2 (target loop thread): take over as the new current loop.
    // No poller registration happens here; the next watch_add re-adds.
    void adopt_loop(fiber::event::EventLoop &loop) noexcept;

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

    fiber::event::EventLoop &owner_loop_;
    fiber::event::EventLoop *current_loop_;
    void *sink_ = nullptr;
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
