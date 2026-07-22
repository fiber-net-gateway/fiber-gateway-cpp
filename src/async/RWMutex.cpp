#include "RWMutex.h"

#include <utility>

#include "../common/Assert.h"

namespace fiber::async {

RWMutex::Waiter::Waiter(RWMutex *owner, WaiterKind kind, std::coroutine_handle<> handle, fiber::event::EventLoop *loop,
                        std::thread::id thread_id, bool *completed) :
    mutex(owner), kind(kind), handle(handle), loop(loop), thread(thread_id), completed(completed) {}

void RWMutex::Waiter::resume() {
    WaiterState expected = WaiterState::Notified;
    if (!state.compare_exchange_strong(expected, WaiterState::Resumed, std::memory_order_acq_rel)) {
        return;
    }
    FIBER_ASSERT(completed != nullptr);
    *completed = true;
    auto resume_handle = handle;
    handle = {};
    if (resume_handle) {
        resume_handle.resume();
    }
}

void RWMutex::Waiter::on_run(Waiter *waiter) noexcept {
    if (!waiter) {
        return;
    }
    waiter->resume();
    delete waiter;
}

void RWMutex::WakeList::push_back(WaiterPtr waiter) noexcept {
    if (!waiter) {
        return;
    }
    waiter->prev = tail;
    waiter->next = nullptr;
    if (tail) {
        tail->next = waiter;
    } else {
        head = waiter;
    }
    tail = waiter;
}

RWMutex::WriteLockGuard::WriteLockGuard(WriteLockGuard &&other) noexcept : mutex_(other.mutex_) {
    other.mutex_ = nullptr;
}

RWMutex::WriteLockGuard &RWMutex::WriteLockGuard::operator=(WriteLockGuard &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (mutex_) {
        mutex_->unlock();
    }
    mutex_ = other.mutex_;
    other.mutex_ = nullptr;
    return *this;
}

RWMutex::WriteLockGuard::~WriteLockGuard() {
    if (mutex_) {
        mutex_->unlock();
    }
}

void RWMutex::WriteLockGuard::unlock() {
    if (!mutex_) {
        return;
    }
    mutex_->unlock();
    mutex_ = nullptr;
}

bool RWMutex::WriteLockGuard::owns_lock() const noexcept { return mutex_ != nullptr; }

RWMutex::ReadLockGuard::ReadLockGuard(ReadLockGuard &&other) noexcept : mutex_(other.mutex_) { other.mutex_ = nullptr; }

RWMutex::ReadLockGuard &RWMutex::ReadLockGuard::operator=(ReadLockGuard &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (mutex_) {
        mutex_->unlock_shared();
    }
    mutex_ = other.mutex_;
    other.mutex_ = nullptr;
    return *this;
}

RWMutex::ReadLockGuard::~ReadLockGuard() {
    if (mutex_) {
        mutex_->unlock_shared();
    }
}

void RWMutex::ReadLockGuard::unlock() {
    if (!mutex_) {
        return;
    }
    mutex_->unlock_shared();
    mutex_ = nullptr;
}

bool RWMutex::ReadLockGuard::owns_lock() const noexcept { return mutex_ != nullptr; }

RWMutex::WriteLockAwaiter::~WriteLockAwaiter() {
    if (mutex_ && waiter_) {
        mutex_->cancel_waiter(waiter_);
    }
}

bool RWMutex::WriteLockAwaiter::await_ready() noexcept {
    if (!mutex_) {
        completed_ = true;
        return true;
    }
    if (mutex_->try_lock()) {
        acquired_ = true;
        completed_ = true;
        return true;
    }
    return false;
}

bool RWMutex::WriteLockAwaiter::await_suspend(std::coroutine_handle<> handle) {
    if (!mutex_ || acquired_) {
        completed_ = true;
        return false;
    }
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    waiter_ = new Waiter(mutex_, WaiterKind::Writer, handle, loop, std::this_thread::get_id(), &completed_);
    if (!mutex_->enqueue_waiter(waiter_)) {
        acquired_ = true;
        completed_ = true;
        delete waiter_;
        waiter_ = nullptr;
        return false;
    }
    return true;
}

RWMutex::WriteLockGuard RWMutex::WriteLockAwaiter::await_resume() noexcept {
    FIBER_ASSERT(completed_);
    waiter_ = nullptr;
    return mutex_ ? WriteLockGuard(mutex_) : WriteLockGuard();
}

RWMutex::ReadLockAwaiter::~ReadLockAwaiter() {
    if (mutex_ && waiter_) {
        mutex_->cancel_waiter(waiter_);
    }
}

bool RWMutex::ReadLockAwaiter::await_ready() noexcept {
    if (!mutex_) {
        completed_ = true;
        return true;
    }
    if (mutex_->try_lock_shared()) {
        acquired_ = true;
        completed_ = true;
        return true;
    }
    return false;
}

bool RWMutex::ReadLockAwaiter::await_suspend(std::coroutine_handle<> handle) {
    if (!mutex_ || acquired_) {
        completed_ = true;
        return false;
    }
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    waiter_ = new Waiter(mutex_, WaiterKind::Reader, handle, loop, std::this_thread::get_id(), &completed_);
    if (!mutex_->enqueue_waiter(waiter_)) {
        acquired_ = true;
        completed_ = true;
        delete waiter_;
        waiter_ = nullptr;
        return false;
    }
    return true;
}

RWMutex::ReadLockGuard RWMutex::ReadLockAwaiter::await_resume() noexcept {
    FIBER_ASSERT(completed_);
    waiter_ = nullptr;
    return mutex_ ? ReadLockGuard(mutex_) : ReadLockGuard();
}

RWMutex::WriteLockAwaiter RWMutex::lock() noexcept { return WriteLockAwaiter(*this); }

RWMutex::ReadLockAwaiter RWMutex::lock_shared() noexcept { return ReadLockAwaiter(*this); }

bool RWMutex::try_lock() noexcept {
    std::lock_guard guard(state_mu_);
    if (writer_locked_ || active_readers_ != 0) {
        return false;
    }
    FIBER_ASSERT(writer_waiters_head_ == nullptr);
    writer_locked_ = true;
    owner_thread_ = std::this_thread::get_id();
    return true;
}

bool RWMutex::try_lock_shared() noexcept {
    std::lock_guard guard(state_mu_);
    if (writer_locked_ || writer_waiters_head_ != nullptr) {
        return false;
    }
    ++active_readers_;
    return true;
}

void RWMutex::unlock() {
    WakeList wake_list;
    {
        std::lock_guard guard(state_mu_);
        FIBER_ASSERT(writer_locked_);
        FIBER_ASSERT(owner_thread_ == std::this_thread::get_id());
        wake_list = unlock_writer_locked();
    }
    post_resume_list(wake_list);
}

void RWMutex::unlock_shared() {
    WakeList wake_list;
    {
        std::lock_guard guard(state_mu_);
        FIBER_ASSERT(active_readers_ > 0);
        wake_list = unlock_reader_locked();
    }
    post_resume_list(wake_list);
}

bool RWMutex::locked() const noexcept {
    std::lock_guard guard(state_mu_);
    return writer_locked_ || active_readers_ != 0;
}

bool RWMutex::write_locked() const noexcept {
    std::lock_guard guard(state_mu_);
    return writer_locked_;
}

std::uint32_t RWMutex::reader_count() const noexcept {
    std::lock_guard guard(state_mu_);
    return active_readers_;
}

bool RWMutex::enqueue_waiter(WaiterPtr waiter) {
    FIBER_ASSERT(waiter != nullptr);
    std::lock_guard guard(state_mu_);
    if (waiter->kind == WaiterKind::Writer) {
        if (!writer_locked_ && active_readers_ == 0) {
            writer_locked_ = true;
            owner_thread_ = waiter->thread;
            return false;
        }
        waiter->prev = writer_waiters_tail_;
        waiter->next = nullptr;
        if (writer_waiters_tail_) {
            writer_waiters_tail_->next = waiter;
        } else {
            writer_waiters_head_ = waiter;
        }
        writer_waiters_tail_ = waiter;
        waiter->queued = true;
        return true;
    }

    if (!writer_locked_ && writer_waiters_head_ == nullptr) {
        ++active_readers_;
        return false;
    }

    waiter->prev = reader_waiters_tail_;
    waiter->next = nullptr;
    if (reader_waiters_tail_) {
        reader_waiters_tail_->next = waiter;
    } else {
        reader_waiters_head_ = waiter;
    }
    reader_waiters_tail_ = waiter;
    waiter->queued = true;
    return true;
}

void RWMutex::cancel_waiter(WaiterPtr waiter) {
    if (!waiter) {
        return;
    }

    WakeList wake_list;
    bool should_delete = false;
    {
        std::lock_guard guard(state_mu_);
        WaiterState state = waiter->state.load(std::memory_order_acquire);
        if (state == WaiterState::Waiting) {
            if (waiter->queued) {
                unlink_waiter_locked(waiter);
            }
            waiter->state.store(WaiterState::Canceled, std::memory_order_release);
            waiter->handle = {};
            should_delete = true;
        } else if (state == WaiterState::Notified) {
            if (waiter->kind == WaiterKind::Writer) {
                FIBER_ASSERT(owner_thread_ == std::this_thread::get_id());
                waiter->state.store(WaiterState::Canceled, std::memory_order_release);
                waiter->handle = {};
                wake_list = select_next_waiters_locked();
            } else {
                FIBER_ASSERT(waiter->thread == std::this_thread::get_id());
                waiter->state.store(WaiterState::Canceled, std::memory_order_release);
                waiter->handle = {};
                FIBER_ASSERT(active_readers_ > 0);
                --active_readers_;
                if (active_readers_ == 0) {
                    wake_list = select_next_waiters_locked();
                }
            }
        }
    }

    post_resume_list(wake_list);
    if (should_delete) {
        delete waiter;
    }
}

RWMutex::WakeList RWMutex::unlock_writer_locked() {
    FIBER_ASSERT(writer_locked_);
    writer_locked_ = false;
    owner_thread_ = {};
    return select_next_waiters_locked();
}

RWMutex::WakeList RWMutex::unlock_reader_locked() {
    FIBER_ASSERT(active_readers_ > 0);
    --active_readers_;
    if (active_readers_ != 0) {
        return {};
    }
    return select_next_waiters_locked();
}

RWMutex::WakeList RWMutex::select_next_waiters_locked() {
    WakeList wake_list;
    while (writer_waiters_head_) {
        WaiterPtr waiter = pop_waiter_locked(writer_waiters_head_, writer_waiters_tail_);
        WaiterState state = waiter->state.load(std::memory_order_acquire);
        FIBER_ASSERT(state != WaiterState::Resumed);
        if (state != WaiterState::Waiting) {
            continue;
        }
        waiter->state.store(WaiterState::Notified, std::memory_order_release);
        writer_locked_ = true;
        owner_thread_ = waiter->thread;
        wake_list.push_back(waiter);
        return wake_list;
    }

    append_reader_batch_locked(wake_list);
    if (wake_list.head == nullptr) {
        writer_locked_ = false;
        owner_thread_ = {};
    }
    return wake_list;
}

RWMutex::WaiterPtr RWMutex::pop_waiter_locked(WaiterPtr &head, WaiterPtr &tail) {
    WaiterPtr waiter = head;
    FIBER_ASSERT(waiter != nullptr);
    head = waiter->next;
    if (head) {
        head->prev = nullptr;
    } else {
        tail = nullptr;
    }
    waiter->prev = nullptr;
    waiter->next = nullptr;
    waiter->queued = false;
    return waiter;
}

void RWMutex::unlink_waiter_locked(WaiterPtr waiter) {
    WaiterPtr &head = waiter->kind == WaiterKind::Writer ? writer_waiters_head_ : reader_waiters_head_;
    WaiterPtr &tail = waiter->kind == WaiterKind::Writer ? writer_waiters_tail_ : reader_waiters_tail_;
    if (waiter->prev) {
        waiter->prev->next = waiter->next;
    } else {
        head = waiter->next;
    }
    if (waiter->next) {
        waiter->next->prev = waiter->prev;
    } else {
        tail = waiter->prev;
    }
    waiter->prev = nullptr;
    waiter->next = nullptr;
    waiter->queued = false;
}

void RWMutex::append_reader_batch_locked(WakeList &wake_list) {
    std::uint32_t resumed = 0;
    while (reader_waiters_head_) {
        WaiterPtr waiter = pop_waiter_locked(reader_waiters_head_, reader_waiters_tail_);
        WaiterState state = waiter->state.load(std::memory_order_acquire);
        FIBER_ASSERT(state != WaiterState::Resumed);
        if (state != WaiterState::Waiting) {
            continue;
        }
        waiter->state.store(WaiterState::Notified, std::memory_order_release);
        wake_list.push_back(waiter);
        ++resumed;
    }
    active_readers_ += resumed;
}

void RWMutex::post_resume(WaiterPtr waiter) {
    FIBER_ASSERT(waiter != nullptr);
    FIBER_ASSERT(waiter->loop != nullptr);
    waiter->loop->post<Waiter, &Waiter::notify_entry, &Waiter::on_run>(*waiter);
}

void RWMutex::post_resume_list(WakeList wake_list) {
    WaiterPtr waiter = wake_list.head;
    while (waiter) {
        WaiterPtr next = waiter->next;
        waiter->prev = nullptr;
        waiter->next = nullptr;
        post_resume(waiter);
        waiter = next;
    }
}

} // namespace fiber::async
