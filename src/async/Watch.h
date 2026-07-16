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

    struct Snapshot {
        std::shared_ptr<const T> value;
        std::uint64_t version = 0;
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

        std::uint64_t subscribe_version() const noexcept {
            std::lock_guard guard(mutex_);
            return version_;
        }

        Snapshot snapshot() const {
            std::lock_guard guard(mutex_);
            return Snapshot{latest_, version_};
        }

        bool version_changed(std::uint64_t observed_version) const noexcept {
            std::lock_guard guard(mutex_);
            return version_ != observed_version;
        }

        bool enqueue_if_unchanged(Waiter *waiter, std::uint64_t observed_version) noexcept {
            FIBER_ASSERT(waiter != nullptr);

            std::lock_guard guard(mutex_);
            if (version_ != observed_version) {
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
            explicit NextAwaiter(Subscriber &subscriber) noexcept :
                subscriber_(&subscriber), state_(subscriber.state_) {
                FIBER_ASSERT(state_ != nullptr);
                FIBER_ASSERT(!subscriber_->next_active_);
                subscriber_->next_active_ = true;
            }

            NextAwaiter(const NextAwaiter &) = delete;
            NextAwaiter &operator=(const NextAwaiter &) = delete;
            NextAwaiter(NextAwaiter &&) = delete;
            NextAwaiter &operator=(NextAwaiter &&) = delete;

            ~NextAwaiter() {
                cancel();
                subscriber_->next_active_ = false;
            }

            bool await_ready() const noexcept { return state_->version_changed(subscriber_->observed_version_); }

            bool await_suspend(std::coroutine_handle<> handle) {
                auto *loop = event::EventLoop::current_or_null();
                FIBER_ASSERT(loop != nullptr);

                waiter_ = new Waiter(loop, handle);
                if (!state_->enqueue_if_unchanged(waiter_, subscriber_->observed_version_)) {
                    delete waiter_;
                    waiter_ = nullptr;
                    return false;
                }
                return true;
            }

            std::shared_ptr<const T> await_resume() {
                waiter_ = nullptr;
                Snapshot snapshot = state_->snapshot();
                subscriber_->observed_version_ = snapshot.version;
                return std::move(snapshot.value);
            }

            void cancel() noexcept {
                if (!waiter_) {
                    return;
                }
                state_->cancel_waiter(waiter_);
                waiter_ = nullptr;
            }

        private:
            Subscriber *subscriber_ = nullptr;
            std::shared_ptr<SharedState> state_;
            Waiter *waiter_ = nullptr;
        };

        Subscriber(const Subscriber &) = delete;
        Subscriber &operator=(const Subscriber &) = delete;

        Subscriber(Subscriber &&other) noexcept :
            state_(std::move(other.state_)), observed_version_(other.observed_version_) {
            FIBER_ASSERT(!other.next_active_);
            other.observed_version_ = 0;
        }

        Subscriber &operator=(Subscriber &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            FIBER_ASSERT(!next_active_);
            FIBER_ASSERT(!other.next_active_);
            state_ = std::move(other.state_);
            observed_version_ = other.observed_version_;
            other.observed_version_ = 0;
            return *this;
        }

        [[nodiscard]] std::shared_ptr<const T> current() {
            FIBER_ASSERT(state_ != nullptr);
            FIBER_ASSERT(!next_active_);

            Snapshot snapshot = state_->snapshot();
            observed_version_ = snapshot.version;
            return std::move(snapshot.value);
        }

        [[nodiscard]] NextAwaiter next() noexcept {
            FIBER_ASSERT(state_ != nullptr);
            FIBER_ASSERT(!next_active_);
            return NextAwaiter(*this);
        }

    private:
        friend class Watch;

        Subscriber(std::shared_ptr<SharedState> state, std::uint64_t observed_version) noexcept :
            state_(std::move(state)), observed_version_(observed_version) {
            FIBER_ASSERT(state_ != nullptr);
        }

        std::shared_ptr<SharedState> state_;
        std::uint64_t observed_version_ = 0;
        bool next_active_ = false;
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
        return Subscriber(state_, state_->subscribe_version());
    }

private:
    std::shared_ptr<SharedState> state_;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_WATCH_H
