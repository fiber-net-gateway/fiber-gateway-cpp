#ifndef FIBER_ASYNC_RWMUTEX_H
#define FIBER_ASYNC_RWMUTEX_H

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <mutex>
#include <thread>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"

namespace fiber::async {

class RWMutex : public common::NonCopyable, public common::NonMovable {
private:
    struct Waiter;
    using WaiterPtr = Waiter *;

public:
    class WriteLockGuard {
    public:
        WriteLockGuard() = default;
        explicit WriteLockGuard(RWMutex *mutex) : mutex_(mutex) {
        }

        WriteLockGuard(const WriteLockGuard &) = delete;
        WriteLockGuard &operator=(const WriteLockGuard &) = delete;
        WriteLockGuard(WriteLockGuard &&other) noexcept;
        WriteLockGuard &operator=(WriteLockGuard &&other) noexcept;
        ~WriteLockGuard();

        void unlock();
        [[nodiscard]] bool owns_lock() const noexcept;

    private:
        RWMutex *mutex_ = nullptr;
    };

    class ReadLockGuard {
    public:
        ReadLockGuard() = default;
        explicit ReadLockGuard(RWMutex *mutex) : mutex_(mutex) {
        }

        ReadLockGuard(const ReadLockGuard &) = delete;
        ReadLockGuard &operator=(const ReadLockGuard &) = delete;
        ReadLockGuard(ReadLockGuard &&other) noexcept;
        ReadLockGuard &operator=(ReadLockGuard &&other) noexcept;
        ~ReadLockGuard();

        void unlock();
        [[nodiscard]] bool owns_lock() const noexcept;

    private:
        RWMutex *mutex_ = nullptr;
    };

    class WriteLockAwaiter {
    public:
        explicit WriteLockAwaiter(RWMutex &mutex) noexcept : mutex_(&mutex) {
        }

        WriteLockAwaiter(const WriteLockAwaiter &) = delete;
        WriteLockAwaiter &operator=(const WriteLockAwaiter &) = delete;
        WriteLockAwaiter(WriteLockAwaiter &&) = delete;
        WriteLockAwaiter &operator=(WriteLockAwaiter &&) = delete;
        ~WriteLockAwaiter();

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        WriteLockGuard await_resume() noexcept;

    private:
        RWMutex *mutex_ = nullptr;
        WaiterPtr waiter_ = nullptr;
        bool acquired_ = false;
    };

    class ReadLockAwaiter {
    public:
        explicit ReadLockAwaiter(RWMutex &mutex) noexcept : mutex_(&mutex) {
        }

        ReadLockAwaiter(const ReadLockAwaiter &) = delete;
        ReadLockAwaiter &operator=(const ReadLockAwaiter &) = delete;
        ReadLockAwaiter(ReadLockAwaiter &&) = delete;
        ReadLockAwaiter &operator=(ReadLockAwaiter &&) = delete;
        ~ReadLockAwaiter();

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        ReadLockGuard await_resume() noexcept;

    private:
        RWMutex *mutex_ = nullptr;
        WaiterPtr waiter_ = nullptr;
        bool acquired_ = false;
    };

    RWMutex() = default;
    ~RWMutex() = default;

    [[nodiscard]] WriteLockAwaiter lock() noexcept;
    [[nodiscard]] ReadLockAwaiter lock_shared() noexcept;
    bool try_lock() noexcept;
    bool try_lock_shared() noexcept;
    void unlock();
    void unlock_shared();
    [[nodiscard]] bool locked() const noexcept;
    [[nodiscard]] bool write_locked() const noexcept;
    [[nodiscard]] std::uint32_t reader_count() const noexcept;

private:
    enum class WaiterKind : std::uint8_t {
        Reader,
        Writer
    };

    enum class WaiterState : std::uint8_t {
        Waiting,
        Notified,
        Resumed,
        Canceled
    };

    struct Waiter {
        Waiter(RWMutex *owner,
               WaiterKind kind,
               std::coroutine_handle<> handle,
               fiber::event::EventLoop *loop,
               std::thread::id thread_id);

        void resume();
        static void on_run(Waiter *waiter);

        RWMutex *mutex = nullptr;
        WaiterKind kind = WaiterKind::Writer;
        std::coroutine_handle<> handle{};
        fiber::event::EventLoop *loop = nullptr;
        std::thread::id thread{};
        std::atomic<WaiterState> state{WaiterState::Waiting};
        Waiter *prev = nullptr;
        Waiter *next = nullptr;
        bool queued = false;
        fiber::event::EventLoop::NotifyEntry notify_entry{};
    };

    struct WakeList {
        WaiterPtr head = nullptr;
        WaiterPtr tail = nullptr;

        void push_back(WaiterPtr waiter) noexcept;
    };

    bool enqueue_waiter(WaiterPtr waiter);
    void cancel_waiter(WaiterPtr waiter);
    WakeList unlock_writer_locked();
    WakeList unlock_reader_locked();
    WakeList select_next_waiters_locked();
    WaiterPtr pop_waiter_locked(WaiterPtr &head, WaiterPtr &tail);
    void unlink_waiter_locked(WaiterPtr waiter);
    void append_reader_batch_locked(WakeList &wake_list);
    static void post_resume(WaiterPtr waiter);
    static void post_resume_list(WakeList wake_list);

    mutable std::mutex state_mu_{};
    WaiterPtr reader_waiters_head_ = nullptr;
    WaiterPtr reader_waiters_tail_ = nullptr;
    WaiterPtr writer_waiters_head_ = nullptr;
    WaiterPtr writer_waiters_tail_ = nullptr;
    std::uint32_t active_readers_ = 0;
    bool writer_locked_ = false;
    std::thread::id owner_thread_{};
};

} // namespace fiber::async

#endif // FIBER_ASYNC_RWMUTEX_H
