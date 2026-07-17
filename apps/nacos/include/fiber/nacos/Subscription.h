#ifndef FIBER_NACOS_SUBSCRIPTION_H
#define FIBER_NACOS_SUBSCRIPTION_H

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <async/Task.h>
#include <async/Watch.h>

namespace fiber::nacos {

// Polymorphic lease base. Public so Subscription<T> can hold/destroy one
// without depending on detail headers; the concrete templated lease derives
// from it in detail. close() is idempotent and owner-loop-only.
struct SubscriptionLeaseBase {
    virtual ~SubscriptionLeaseBase() = default;
    virtual void close() noexcept = 0;
};

enum class ResultKind : std::uint8_t { Closed, Success };

template<typename T>
struct SubscriptionResult {
    ResultKind kind = ResultKind::Success;
    typename async::Watch<T>::Snapshot snapshot;
};

// Zero-allocation awaiter wrapping Watch::Subscriber::NextAwaiter. On resume it
// checks the entry's closed flag: if the subscription was closed (pool
// shutdown) the result kind is Closed and the snapshot value is ignored.
//
// Like Watch::NextAwaiter it is non-movable: the inner NextAwaiter is
// non-movable, so it is direct-initialized (copy-elided) in the member init
// list from a factory that returns the NextAwaiter prvalue. The whole awaiter
// is only ever a prvalue (returned from Subscription::next).
template<typename T>
class SubscriptionNextAwaiter {
public:
    template<typename Factory>
        requires std::invocable<Factory &> && std::same_as<std::remove_cvref_t<std::invoke_result_t<Factory &>>,
                                                           typename async::Watch<T>::Subscriber::NextAwaiter>
    SubscriptionNextAwaiter(Factory &&factory, const std::atomic<bool> *closed) noexcept :
        inner_(std::invoke(factory)), closed_(closed) {}

    SubscriptionNextAwaiter(const SubscriptionNextAwaiter &) = delete;
    SubscriptionNextAwaiter &operator=(const SubscriptionNextAwaiter &) = delete;
    SubscriptionNextAwaiter(SubscriptionNextAwaiter &&) = delete;
    SubscriptionNextAwaiter &operator=(SubscriptionNextAwaiter &&) = delete;

    ~SubscriptionNextAwaiter() { inner_.cancel(); }

    [[nodiscard]] bool await_ready() const noexcept { return inner_.await_ready(); }

    bool await_suspend(std::coroutine_handle<> handle) { return inner_.await_suspend(handle); }

    SubscriptionResult<T> await_resume() {
        auto snapshot = inner_.await_resume();
        const bool closed = closed_ != nullptr && closed_->load(std::memory_order_acquire);
        return SubscriptionResult<T>{
                .kind = closed ? ResultKind::Closed : ResultKind::Success,
                .snapshot = std::move(snapshot),
        };
    }

private:
    typename async::Watch<T>::Subscriber::NextAwaiter inner_;
    const std::atomic<bool> *closed_;
};

// Public subscription handle. Template on the value type only; the entry type
// is erased behind the lease. Move-only. close()/destruction is owner-loop-only
// (the lease must not be touched from another loop). next() may be awaited from
// any loop.
template<typename T>
class Subscription {
public:
    using Snapshot = typename async::Watch<T>::Snapshot;

    // Public only because the templated constructor cannot be hidden behind a
    // friend without leaking detail types; callers cannot construct one without
    // the internal lease + closed flag, which only the pool produces.
    Subscription(std::unique_ptr<SubscriptionLeaseBase> lease, typename async::Watch<T>::Subscriber subscriber,
                 const std::atomic<bool> *closed_flag) noexcept :
        lease_(std::move(lease)), subscriber_(std::move(subscriber)), closed_flag_(closed_flag) {}

    Subscription(Subscription &&other) noexcept = default;
    Subscription &operator=(Subscription &&other) noexcept {
        if (this != &other) {
            close();
            lease_ = std::move(other.lease_);
            subscriber_ = std::move(other.subscriber_);
            closed_flag_ = other.closed_flag_;
            other.closed_flag_ = nullptr;
        }
        return *this;
    }
    ~Subscription() { close(); }

    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;

    // Last published value, or null if nothing has been published yet
    // (never-synced). Does not carry the Close signal.
    [[nodiscard]] Snapshot current() const noexcept {
        FIBER_ASSERT(subscriber_.has_value());
        return subscriber_->current();
    }

    // True once the pool has closed this subscription (shutdown). Readable from
    // any loop.
    [[nodiscard]] bool closed() const noexcept {
        return closed_flag_ != nullptr && closed_flag_->load(std::memory_order_acquire);
    }

    // Block (co_await) until a value newer than received_version is published.
    // Returns a Result; kind is Closed when the pool shut the subscription down.
    [[nodiscard]] SubscriptionNextAwaiter<T> next(std::uint64_t received_version) const noexcept {
        FIBER_ASSERT(subscriber_.has_value());
        return SubscriptionNextAwaiter<T>(
                [&subscriber = *subscriber_, received_version]() { return subscriber.next(received_version); },
                closed_flag_);
    }

    // Release the subscriber reference. Idempotent.
    void close() noexcept {
        if (lease_) {
            lease_->close();
            lease_.reset();
        }
        subscriber_.reset();
        closed_flag_ = nullptr;
    }

private:
    std::unique_ptr<SubscriptionLeaseBase> lease_;
    std::optional<typename async::Watch<T>::Subscriber> subscriber_;
    const std::atomic<bool> *closed_flag_ = nullptr;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_SUBSCRIPTION_H
