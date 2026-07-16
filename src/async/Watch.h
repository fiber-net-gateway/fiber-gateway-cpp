#ifndef FIBER_ASYNC_WATCH_H
#define FIBER_ASYNC_WATCH_H

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "../common/Assert.h"
#include "../event/EventLoop.h"

namespace fiber::async {

template<typename T>
class Watch {
public:
    struct Snapshot {
        std::shared_ptr<const T> value;
        std::uint64_t version = 0;
    };

private:
    enum class WaiterState : std::uint8_t { Waiting, Notified, Resumed, Canceled };

    struct Waiter {
        Waiter(event::EventLoop *loop, std::coroutine_handle<> handle) noexcept : loop(loop), handle(handle) {}

        void resume() noexcept {
            WaiterState expected = WaiterState::Notified;
            if (!state.compare_exchange_strong(expected, WaiterState::Resumed, std::memory_order_acq_rel)) {
                return;
            }

            auto resume_handle = handle;
            handle = {};
            if (resume_handle) {
                resume_handle.resume();
            }
        }

        static void on_run(Waiter *waiter) noexcept {
            FIBER_ASSERT(waiter != nullptr);
            waiter->resume();
            delete waiter;
        }

        event::EventLoop *loop = nullptr;
        std::coroutine_handle<> handle{};
        std::atomic<WaiterState> state{WaiterState::Waiting};
        Waiter *prev = nullptr;
        Waiter *next = nullptr;
        bool queued = false;
        event::EventLoop::NotifyEntry notify_entry{};
    };

    struct SharedState {
        SharedState() = default;

        explicit SharedState(T initial_value) : latest_(std::make_shared<T>(std::move(initial_value))), version_(1) {}

        ~SharedState() {
            FIBER_ASSERT(waiters_head_ == nullptr);
            FIBER_ASSERT(waiters_tail_ == nullptr);
        }

        SharedState(const SharedState &) = delete;
        SharedState &operator=(const SharedState &) = delete;

        bool try_acquire_publisher() noexcept {
            std::lock_guard guard(mutex_);
            if (publisher_acquired_) {
                return false;
            }
            publisher_acquired_ = true;
            return true;
        }

        Snapshot snapshot() const {
            std::lock_guard guard(mutex_);
            return Snapshot{latest_, version_};
        }

        Snapshot snapshot_after(std::uint64_t received_version) const {
            std::lock_guard guard(mutex_);
            FIBER_ASSERT(received_version < version_);
            return Snapshot{latest_, version_};
        }

        bool has_newer_version(std::uint64_t received_version) const noexcept {
            std::lock_guard guard(mutex_);
            FIBER_ASSERT(received_version <= version_);
            return received_version < version_;
        }

        bool enqueue_if_current(Waiter *waiter, std::uint64_t received_version) noexcept {
            FIBER_ASSERT(waiter != nullptr);

            std::lock_guard guard(mutex_);
            FIBER_ASSERT(received_version <= version_);
            if (received_version < version_) {
                return false;
            }

            waiter->prev = waiters_tail_;
            if (waiters_tail_) {
                waiters_tail_->next = waiter;
            } else {
                waiters_head_ = waiter;
            }
            waiters_tail_ = waiter;
            waiter->queued = true;
            return true;
        }

        void cancel_waiter(Waiter *waiter) noexcept {
            FIBER_ASSERT(waiter != nullptr);

            bool should_delete = false;
            {
                std::lock_guard guard(mutex_);
                const WaiterState state = waiter->state.load(std::memory_order_acquire);
                if (state == WaiterState::Waiting) {
                    if (waiter->queued) {
                        unlink_waiter(waiter);
                    }
                    waiter->handle = {};
                    waiter->state.store(WaiterState::Canceled, std::memory_order_release);
                    should_delete = true;
                } else if (state == WaiterState::Notified) {
                    waiter->handle = {};
                    waiter->state.store(WaiterState::Canceled, std::memory_order_release);
                }
            }

            if (should_delete) {
                delete waiter;
            }
        }

        void publish(std::shared_ptr<const T> value) {
            FIBER_ASSERT(value != nullptr);

            Waiter *notify_head = nullptr;
            {
                std::lock_guard guard(mutex_);
                FIBER_ASSERT(version_ != std::numeric_limits<std::uint64_t>::max());

                latest_.swap(value);
                ++version_;

                notify_head = waiters_head_;
                waiters_head_ = nullptr;
                waiters_tail_ = nullptr;

                for (Waiter *waiter = notify_head; waiter; waiter = waiter->next) {
                    waiter->prev = nullptr;
                    waiter->queued = false;
                    waiter->state.store(WaiterState::Notified, std::memory_order_release);
                }
            }

            while (notify_head) {
                Waiter *waiter = notify_head;
                notify_head = waiter->next;
                waiter->next = nullptr;
                post_resume(waiter);
            }
        }

    private:
        void unlink_waiter(Waiter *waiter) noexcept {
            FIBER_ASSERT(waiter->queued);

            if (waiter->prev) {
                waiter->prev->next = waiter->next;
            } else {
                waiters_head_ = waiter->next;
            }
            if (waiter->next) {
                waiter->next->prev = waiter->prev;
            } else {
                waiters_tail_ = waiter->prev;
            }

            waiter->prev = nullptr;
            waiter->next = nullptr;
            waiter->queued = false;
        }

        static void post_resume(Waiter *waiter) noexcept {
            FIBER_ASSERT(waiter != nullptr);
            FIBER_ASSERT(waiter->loop != nullptr);
            waiter->loop->template post<Waiter, &Waiter::notify_entry, &Waiter::on_run>(*waiter);
        }

        mutable std::mutex mutex_{};
        std::shared_ptr<const T> latest_{};
        std::uint64_t version_ = 0;
        bool publisher_acquired_ = false;
        Waiter *waiters_head_ = nullptr;
        Waiter *waiters_tail_ = nullptr;
    };

public:
    class Publisher {
    public:
        Publisher(const Publisher &) = delete;
        Publisher &operator=(const Publisher &) = delete;
        Publisher(Publisher &&) noexcept = default;
        Publisher &operator=(Publisher &&) noexcept = default;

        void publish(T value) {
            FIBER_ASSERT(state_ != nullptr);
            state_->publish(std::make_shared<T>(std::move(value)));
        }

    private:
        friend class Watch;

        explicit Publisher(std::shared_ptr<SharedState> state) noexcept : state_(std::move(state)) {
            FIBER_ASSERT(state_ != nullptr);
        }

        std::shared_ptr<SharedState> state_;
    };

    class Subscriber {
    public:
        class NextAwaiter {
        public:
            NextAwaiter(std::shared_ptr<SharedState> state, std::uint64_t received_version) noexcept :
                state_(std::move(state)), received_version_(received_version) {
                FIBER_ASSERT(state_ != nullptr);
            }

            NextAwaiter(const NextAwaiter &) = delete;
            NextAwaiter &operator=(const NextAwaiter &) = delete;
            NextAwaiter(NextAwaiter &&) = delete;
            NextAwaiter &operator=(NextAwaiter &&) = delete;

            ~NextAwaiter() { cancel(); }

            bool await_ready() const noexcept { return state_->has_newer_version(received_version_); }

            bool await_suspend(std::coroutine_handle<> handle) {
                auto *loop = event::EventLoop::current_or_null();
                FIBER_ASSERT(loop != nullptr);

                waiter_ = new Waiter(loop, handle);
                if (!state_->enqueue_if_current(waiter_, received_version_)) {
                    delete waiter_;
                    waiter_ = nullptr;
                    return false;
                }
                return true;
            }

            Snapshot await_resume() {
                waiter_ = nullptr;
                return state_->snapshot_after(received_version_);
            }

            void cancel() noexcept {
                if (!waiter_) {
                    return;
                }
                state_->cancel_waiter(waiter_);
                waiter_ = nullptr;
            }

        private:
            std::shared_ptr<SharedState> state_;
            std::uint64_t received_version_ = 0;
            Waiter *waiter_ = nullptr;
        };

        Subscriber(const Subscriber &) = delete;
        Subscriber &operator=(const Subscriber &) = delete;
        Subscriber(Subscriber &&) noexcept = default;
        Subscriber &operator=(Subscriber &&) noexcept = default;

        [[nodiscard]] Snapshot current() const {
            FIBER_ASSERT(state_ != nullptr);
            return state_->snapshot();
        }

        [[nodiscard]] NextAwaiter next(std::uint64_t received_version) const noexcept {
            FIBER_ASSERT(state_ != nullptr);
            return NextAwaiter(state_, received_version);
        }

    private:
        friend class Watch;

        explicit Subscriber(std::shared_ptr<SharedState> state) noexcept : state_(std::move(state)) {
            FIBER_ASSERT(state_ != nullptr);
        }

        std::shared_ptr<SharedState> state_;
    };

    Watch() : state_(std::make_shared<SharedState>()) {}

    explicit Watch(T initial_value) : state_(std::make_shared<SharedState>(std::move(initial_value))) {}

    Watch(const Watch &) = delete;
    Watch &operator=(const Watch &) = delete;
    Watch(Watch &&) noexcept = default;
    Watch &operator=(Watch &&) noexcept = default;

    [[nodiscard]] std::optional<Publisher> acquire_publisher() noexcept {
        FIBER_ASSERT(state_ != nullptr);
        if (!state_->try_acquire_publisher()) {
            return std::nullopt;
        }
        return Publisher(state_);
    }

    [[nodiscard]] Subscriber subscribe() {
        FIBER_ASSERT(state_ != nullptr);
        return Subscriber(state_);
    }

private:
    std::shared_ptr<SharedState> state_;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_WATCH_H
