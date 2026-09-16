#include <fiber/net/detail/Efd.h>

#include <unistd.h>

namespace fiber::net::detail {

Efd::Efd(fiber::event::EventLoop &owner_loop, void *sink, EventCallback callback,
         fiber::event::Poller::Mode mode) noexcept :
    owner_loop_(owner_loop), current_loop_(&owner_loop), sink_(sink), callback_(callback), mode_(mode) {
    item_.efd = this;
    item_.callback = &Efd::on_poller_event;
}

Efd::~Efd() {
    if (fd_ < 0) {
        return;
    }

    if (registered_) {
        if (!current_loop_->in_loop()) {
            FIBER_ASSERT(false);
            return;
        }
        close_fd();
        return;
    }

    int fd = fd_;
    fd_ = -1;
    watching_ = fiber::event::IoEvent::None;
    ::close(fd);
}

void Efd::set_event_sink(void *sink, EventCallback callback) noexcept {
    sink_ = sink;
    callback_ = callback;
}

fiber::common::IoErr Efd::attach(int fd) noexcept {
    if (fd_ >= 0) {
        return fiber::common::IoErr::Already;
    }
    if (fd < 0) {
        return fiber::common::IoErr::Invalid;
    }
    FIBER_ASSERT(!registered_);
    FIBER_ASSERT(watching_ == fiber::event::IoEvent::None);
    fd_ = fd;
    return fiber::common::IoErr::None;
}

int Efd::release_fd() noexcept {
    FIBER_ASSERT(!registered_);
    FIBER_ASSERT(watching_ == fiber::event::IoEvent::None);
    int fd = fd_;
    fd_ = -1;
    return fd;
}

void Efd::close_fd() noexcept {
    if (fd_ < 0) {
        return;
    }
    if (registered_) {
        FIBER_ASSERT(current_loop_->in_loop());
        // Closing the fd removes the kernel registration even if this DEL
        // fails (e.g. EBADF from a racing close), so the error is dropped.
        (void) current_loop_->poller().del(item_);
        registered_ = false;
        ++epoch_;
    }
    int fd = fd_;
    fd_ = -1;
    watching_ = fiber::event::IoEvent::None;
    ::close(fd);
}

fiber::common::IoErr Efd::detach_from_current_loop() noexcept {
    if (!registered_) {
        watching_ = fiber::event::IoEvent::None;
        return fiber::common::IoErr::None;
    }
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    FIBER_ASSERT(current_loop_->in_loop());
    fiber::common::IoErr err = current_loop_->poller().del(item_);
    // The poller batch no longer references this item either way; on failure
    // the kernel registration is unknown, so callers must close the fd instead
    // of publishing the object.
    registered_ = false;
    ++epoch_;
    watching_ = fiber::event::IoEvent::None;
    if (err != fiber::common::IoErr::None) {
        return err;
    }
    return fiber::common::IoErr::None;
}

void Efd::adopt_loop(fiber::event::EventLoop &loop) noexcept {
    FIBER_ASSERT(loop.in_loop());
    FIBER_ASSERT(!registered_);
    FIBER_ASSERT(watching_ == fiber::event::IoEvent::None);
    current_loop_ = &loop;
}

fiber::common::IoErr Efd::unwatch_all() noexcept {
    if (!registered_) {
        watching_ = fiber::event::IoEvent::None;
        return fiber::common::IoErr::None;
    }
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    FIBER_ASSERT(current_loop_->in_loop());
    fiber::common::IoErr err = current_loop_->poller().del(item_);
    if (err != fiber::common::IoErr::None) {
        return err;
    }
    registered_ = false;
    ++epoch_;
    watching_ = fiber::event::IoEvent::None;
    return fiber::common::IoErr::None;
}

fiber::common::IoErr Efd::watch_set(fiber::event::IoEvent desired) noexcept {
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    FIBER_ASSERT(current_loop_->in_loop());

    if (desired == watching_ && (fiber::event::any(desired) || !registered_)) {
        return fiber::common::IoErr::None;
    }

    if (!fiber::event::any(desired)) {
        if (!registered_) {
            watching_ = fiber::event::IoEvent::None;
            return fiber::common::IoErr::None;
        }
        fiber::common::IoErr err = current_loop_->poller().del(item_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        registered_ = false;
        ++epoch_;
        watching_ = fiber::event::IoEvent::None;
        return fiber::common::IoErr::None;
    }

    fiber::common::IoErr err = fiber::common::IoErr::None;
    if (!registered_) {
        err = current_loop_->poller().add(fd_, desired, &item_, mode_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        registered_ = true;
        ++epoch_;
    } else {
        err = current_loop_->poller().mod(fd_, desired, &item_, mode_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
    }
    watching_ = desired;
    return fiber::common::IoErr::None;
}

fiber::common::IoErr Efd::watch_add(fiber::event::IoEvent events) noexcept { return watch_set(watching_ | events); }

fiber::common::IoErr Efd::watch_del(fiber::event::IoEvent events) noexcept { return watch_set(watching_ & ~events); }

void Efd::on_poller_event(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events) {
    (void) fd;
    auto *efd_item = static_cast<Item *>(item);
    if (!efd_item || !efd_item->efd) {
        return;
    }
    Efd *efd = efd_item->efd;
    if (!efd->callback_) {
        return;
    }
    efd->callback_(efd->sink_, events);
}

} // namespace fiber::net::detail
