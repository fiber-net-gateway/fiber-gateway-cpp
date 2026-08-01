#ifndef FIBER_TESTS_NACOS_SUBSCRIPTION_STUB_H
#define FIBER_TESTS_NACOS_SUBSCRIPTION_STUB_H

#include <cstddef>
#include <optional>
#include <utility>

#include <common/Assert.h>
#include <fiber/nacos/Subscription.h>

namespace fiber::tests {

// Owner-loop-only callback subscription stub for application tests. Nodes are
// owned by their Subscription handles; this store only links them while they
// are active. The notification cursor is repaired when the pending next node
// is removed, matching the production SubscriptionPool traversal contract.
template<typename T>
class NacosSubscriptionStub {
public:
    using Subscription = nacos::Subscription<T>;
    using Result = nacos::SubscriptionResult<T>;
    using NotifyCallback = typename Subscription::NotifyCallback;

    NacosSubscriptionStub() = default;
    NacosSubscriptionStub(const NacosSubscriptionStub &) = delete;
    NacosSubscriptionStub &operator=(const NacosSubscriptionStub &) = delete;

    ~NacosSubscriptionStub() {
        while (head_ != nullptr) {
            Node *node = head_;
            unlink(*node);
            node->owner = nullptr;
            node->closed = true;
        }
    }

    [[nodiscard]] Subscription subscribe(NotifyCallback callback, void *ctx) {
        FIBER_ASSERT(callback != nullptr);
        Node *node = new Node{
                .owner = this,
                .callback = callback,
                .ctx = ctx,
        };
        link_back(*node);
        ++subscription_count_;
        if (latest_) {
            callback(ctx, *latest_);
        }
        return Subscription(node, &NacosSubscriptionStub::close_node, &NacosSubscriptionStub::node_closed);
    }

    void publish(Result result) {
        latest_ = std::move(result);
        Node *node = head_;
        while (node != nullptr) {
            Node *next = node->next;
            Node **previous_guard = notify_next_ptr_;
            notify_next_ptr_ = &next;
            node->callback(node->ctx, *latest_);
            notify_next_ptr_ = previous_guard;
            node = next;
        }
    }

    [[nodiscard]] std::size_t subscription_count() const noexcept { return subscription_count_; }

private:
    struct Node {
        NacosSubscriptionStub *owner = nullptr;
        NotifyCallback callback = nullptr;
        void *ctx = nullptr;
        Node *previous = nullptr;
        Node *next = nullptr;
        bool closed = false;
    };

    static void close_node(void *value) noexcept {
        auto *node = static_cast<Node *>(value);
        if (node->owner != nullptr) {
            node->owner->remove(*node);
        }
        node->closed = true;
        delete node;
    }

    [[nodiscard]] static bool node_closed(const void *value) noexcept {
        return static_cast<const Node *>(value)->closed;
    }

    void remove(Node &node) noexcept {
        if (notify_next_ptr_ != nullptr && *notify_next_ptr_ == &node) {
            *notify_next_ptr_ = node.next;
        }
        unlink(node);
        node.owner = nullptr;
    }

    void link_back(Node &node) noexcept {
        node.previous = tail_;
        if (tail_ != nullptr) {
            tail_->next = &node;
        } else {
            head_ = &node;
        }
        tail_ = &node;
    }

    void unlink(Node &node) noexcept {
        if (node.previous != nullptr) {
            node.previous->next = node.next;
        } else {
            head_ = node.next;
        }
        if (node.next != nullptr) {
            node.next->previous = node.previous;
        } else {
            tail_ = node.previous;
        }
        node.previous = nullptr;
        node.next = nullptr;
    }

    Node *head_ = nullptr;
    Node *tail_ = nullptr;
    Node **notify_next_ptr_ = nullptr;
    std::optional<Result> latest_;
    std::size_t subscription_count_ = 0;
};

} // namespace fiber::tests

#endif // FIBER_TESTS_NACOS_SUBSCRIPTION_STUB_H
