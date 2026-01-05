#ifndef FIBER_EVENT_MPSC_QUEUE_H
#define FIBER_EVENT_MPSC_QUEUE_H

#include <atomic>
#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "../common/Assert.h"

namespace fiber::event {

template <typename T>
class MpscQueue {
public:
    class Node {
    public:
        explicit Node(const T &value) noexcept(std::is_nothrow_copy_constructible_v<T>) : value_(value) {}
        explicit Node(T &&value) noexcept(std::is_nothrow_move_constructible_v<T>) : value_(std::move(value)) {}

        Node(const Node &) = delete;
        Node &operator=(const Node &) = delete;

    private:
        friend class MpscQueue;

        [[no_unique_address]] T value_;
        Node *next_ = nullptr;
    };

    MpscQueue() = default;
    MpscQueue(const MpscQueue &) = delete;
    MpscQueue &operator=(const MpscQueue &) = delete;

    void push(Node *node) noexcept {
        FIBER_ASSERT(node);
        FIBER_ASSERT(node->next_ == nullptr);

        Node *stale_head = head_.load(std::memory_order_relaxed);
        for (;;) {
            node->next_ = stale_head;
            if (head_.compare_exchange_weak(stale_head, node,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
                return;
            }
        }
    }

    Node *try_pop_all() noexcept {
        Node *last = head_.exchange(nullptr, std::memory_order_consume);
        Node *first = nullptr;
        while (last) {
            Node *tmp = last;
            last = last->next_;
            tmp->next_ = first;
            first = tmp;
        }
        return first;
    }

    static T &unwrap(Node *node) noexcept {
        FIBER_ASSERT(node);
        return node->value_;
    }

    static Node *next(Node *node) noexcept {
        return node ? node->next_ : nullptr;
    }

    template <std::invocable<T &> F>
    static void for_each(Node *root, F &&func) noexcept(std::is_nothrow_invocable_v<F, T &>) {
        while (root) {
            Node *next = root->next_;
            std::invoke(func, root->value_);
            root = next;
        }
    }

private:
    std::atomic<Node *> head_ = nullptr;
};

} // namespace fiber::event

#endif // FIBER_EVENT_MPSC_QUEUE_H
