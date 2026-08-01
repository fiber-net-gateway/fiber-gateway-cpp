#ifndef FIBER_NACOS_SUBSCRIPTION_H
#define FIBER_NACOS_SUBSCRIPTION_H

#include <cstdint>
#include <memory>
#include <utility>

namespace fiber::nacos {

enum class ResultKind : std::uint8_t { Success, Closed };

template<typename T>
struct SubscriptionResult {
    ResultKind kind = ResultKind::Success;
    std::shared_ptr<const T> data;
};

// Move-only RAII handle for one callback registration. The callback node and
// its protocol-specific entry are type-erased so this public header does not
// expose SubscriptionPool internals. Moving the handle only moves a pointer;
// the intrusive-list node itself therefore keeps a stable address.
//
// close()/destruction and callbacks are owner-EventLoop-only. A Closed result
// is delivered before shutdown detaches the node from its entry. Detached nodes
// remain owned by this handle and are freed when it is closed or destroyed.
// The Result reference is valid only for the duration of the callback. Its
// shared data may be copied and retained beyond the callback.
template<typename T>
class Subscription {
public:
    using Result = SubscriptionResult<T>;
    using NotifyCallback = void (*)(void *ctx, const Result &result) noexcept;
    using CloseFn = void (*)(void *node) noexcept;
    using ClosedFn = bool (*)(const void *node) noexcept;

    Subscription() noexcept = default;
    Subscription(void *node, CloseFn close, ClosedFn closed) noexcept : node_(node), close_(close), closed_(closed) {}

    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;

    Subscription(Subscription &&other) noexcept :
        node_(std::exchange(other.node_, nullptr)), close_(std::exchange(other.close_, nullptr)),
        closed_(std::exchange(other.closed_, nullptr)) {}

    Subscription &operator=(Subscription &&other) noexcept {
        if (this != &other) {
            close();
            node_ = std::exchange(other.node_, nullptr);
            close_ = std::exchange(other.close_, nullptr);
            closed_ = std::exchange(other.closed_, nullptr);
        }
        return *this;
    }

    ~Subscription() { close(); }

    void close() noexcept {
        CloseFn close = std::exchange(close_, nullptr);
        void *node = std::exchange(node_, nullptr);
        closed_ = nullptr;
        if (close != nullptr) {
            close(node);
        }
    }

    [[nodiscard]] bool closed() const noexcept { return node_ == nullptr || closed_ == nullptr || closed_(node_); }
    [[nodiscard]] explicit operator bool() const noexcept { return node_ != nullptr; }

private:
    void *node_ = nullptr;
    CloseFn close_ = nullptr;
    ClosedFn closed_ = nullptr;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_SUBSCRIPTION_H
