#ifndef FIBER_NACOS_SUBSCRIPTION_H
#define FIBER_NACOS_SUBSCRIPTION_H

#include <cstdint>
#include <optional>
#include <utility>

#include <async/Task.h>
#include <async/Watch.h>

namespace fiber::nacos {

// The value carried by a subscription Watch. kind distinguishes a real data
// push (Success, with the latest T) from end-of-subscription (Closed, no data).
// This keeps T itself free of any lifecycle state: ConfigData stays pure
// (Present/NotFound), and ServiceInfo need not invent a Closed variant. Closed
// is the moral equivalent of Java Observable.onComplete.
enum class ResultKind : std::uint8_t { Success, Closed };

template<typename T>
struct SubscriptionResult {
    ResultKind kind = ResultKind::Success;
    std::optional<T> data;
};

// RAII handle for one subscriber's reference into a pool entry. Holds a bare
// context pointer plus a type-erased release function: the entry type (which
// carries the protocol-specific state) is erased so the public Subscription<T>
// never names it. The subscriber reference it represents was already counted
// into the entry's ref_count by SubscriptionPool::subscribe; close() drops it.
//
// This is an inline value member of Subscription<T> (no heap allocation, no
// virtual base). The context pointer is the entry; the release function is a
// stateless trampoline the pool installs at subscribe time. Owner-loop-only;
// idempotent.
template<typename T>
class SubscriptionLease {
public:
    using ReleaseFn = void (*)(void *ctx) noexcept;

    SubscriptionLease() noexcept = default;
    SubscriptionLease(void *ctx, ReleaseFn release) noexcept : ctx_(ctx), release_(release) {}

    SubscriptionLease(const SubscriptionLease &) = delete;
    SubscriptionLease &operator=(const SubscriptionLease &) = delete;

    SubscriptionLease(SubscriptionLease &&other) noexcept : ctx_(other.ctx_), release_(other.release_) {
        other.ctx_ = nullptr;
        other.release_ = nullptr;
    }
    SubscriptionLease &operator=(SubscriptionLease &&other) noexcept {
        if (this != &other) {
            close();
            ctx_ = other.ctx_;
            release_ = other.release_;
            other.ctx_ = nullptr;
            other.release_ = nullptr;
        }
        return *this;
    }
    ~SubscriptionLease() { close(); }

    // Drop the subscriber reference. Idempotent.
    void close() noexcept {
        if (release_ != nullptr) {
            ReleaseFn fn = release_;
            void *ctx = ctx_;
            ctx_ = nullptr;
            release_ = nullptr;
            fn(ctx);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept { return release_ != nullptr; }

private:
    void *ctx_ = nullptr;
    ReleaseFn release_ = nullptr;
};

// Public subscription handle. The watched value type is SubscriptionResult<T>,
// so Close is delivered as an ordinary published value (no separate channel,
// no closed flag, no custom awaiter). Subscription is a thin RAII owner: it
// holds a lease (subscriber reference count) and a Watch subscriber. Users
// drive the stream directly through subscriber().current() / .next(version).
//
// Move-only. close()/destruction is owner-loop-only (the lease must not be
// touched from another loop). next() may be awaited from any loop.
template<typename T>
class Subscription {
public:
    using Result = SubscriptionResult<T>;
    using Subscriber = typename async::Watch<Result>::Subscriber;
    using Snapshot = typename async::Watch<Result>::Snapshot;
    using Lease = SubscriptionLease<T>;

    // Public only because the templated constructor cannot be hidden behind a
    // friend without leaking detail types; callers cannot construct one without
    // the internal lease, which only the pool produces.
    Subscription(Lease lease, Subscriber subscriber) noexcept :
        lease_(std::move(lease)), subscriber_(std::move(subscriber)) {}

    Subscription(Subscription &&other) noexcept = default;
    Subscription &operator=(Subscription &&other) noexcept {
        if (this != &other) {
            close();
            lease_ = std::move(other.lease_);
            subscriber_ = std::move(other.subscriber_);
        }
        return *this;
    }
    ~Subscription() { close(); }

    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;

    // The underlying Watch subscriber. current() is null until the first push;
    // next(version) blocks for the next push. A push with kind == Closed marks
    // end-of-subscription (after which no further values arrive).
    [[nodiscard]] Subscriber &subscriber() noexcept { return *subscriber_; }
    [[nodiscard]] const Subscriber &subscriber() const noexcept { return *subscriber_; }

    // True once the pool has published Closed for this subscription. Readable
    // from any loop. Convenience over inspecting current().
    [[nodiscard]] bool closed() const noexcept {
        const auto snapshot = subscriber_->current();
        return snapshot.value != nullptr && snapshot.value->kind == ResultKind::Closed;
    }

    // Release the subscriber reference. Idempotent. After close(), subscriber()
    // must not be used.
    void close() noexcept {
        lease_.close();
        subscriber_.reset();
    }

private:
    Lease lease_;
    std::optional<Subscriber> subscriber_;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_SUBSCRIPTION_H
