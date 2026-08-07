#ifndef FIBER_COMMON_BINARY_HEAP_H
#define FIBER_COMMON_BINARY_HEAP_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fiber::common {

// Intrusive hook for BinaryHeap. Layout matches the historical TimerQueue::Node
// (left/right/parent pointers) so existing owners keep their offsetof offset.
struct BinaryHeapNode {
    BinaryHeapNode *left = nullptr;
    BinaryHeapNode *right = nullptr;
    BinaryHeapNode *parent = nullptr;
};

// Intrusive binary min-heap. Mirrors the IntrusiveRbTree contract:
//   - T must be standard-layout (Offset is used for container_of).
//   - Offset is offsetof(T, hook_member).
//   - Compare is default-constructible; compare_(a, b) returns true when owner
//     a is strictly less than owner b (both pointers are non-null at call sites).
// The heap never owns nodes and never touches owner state beyond the hook.
template<typename T, std::size_t Offset, typename Compare>
class BinaryHeap {
    static_assert(std::is_standard_layout_v<T>,
                  "BinaryHeap owner type must be standard-layout because Offset is used for container_of.");

public:
    BinaryHeap() noexcept = default;

    BinaryHeap(const BinaryHeap &) = delete;
    BinaryHeap &operator=(const BinaryHeap &) = delete;
    BinaryHeap(BinaryHeap &&) = delete;
    BinaryHeap &operator=(BinaryHeap &&) = delete;

    [[nodiscard]] T *min() noexcept { return min_ ? owner_from_hook(min_) : nullptr; }
    [[nodiscard]] const T *min() const noexcept { return min_ ? owner_from_hook(min_) : nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    void insert(T &owner) noexcept {
        BinaryHeapNode *node = &hook_of(owner);
        BinaryHeapNode **parent;
        BinaryHeapNode **child;
        std::size_t path;
        std::size_t n;
        std::size_t k;

        node->left = nullptr;
        node->right = nullptr;
        node->parent = nullptr;

        path = 0;
        for (k = 0, n = 1 + count_; n >= 2; k += 1, n /= 2) {
            path = (path << 1) | (n & 1);
        }

        parent = child = &min_;
        while (k > 0) {
            parent = child;
            if (path & 1) {
                child = &(*child)->right;
            } else {
                child = &(*child)->left;
            }
            path >>= 1;
            k -= 1;
        }

        node->parent = *parent;
        *child = node;
        count_ += 1;

        while (node->parent && compare_(owner_from_hook(node), owner_from_hook(node->parent))) {
            swap_nodes(this, node->parent, node);
        }
    }

    void remove(T &owner) noexcept {
        BinaryHeapNode *node = &hook_of(owner);
        BinaryHeapNode *smallest;
        BinaryHeapNode **max;
        BinaryHeapNode *child;
        std::size_t path;
        std::size_t k;
        std::size_t n;

        if (count_ == 0) {
            return;
        }

        path = 0;
        for (k = 0, n = count_; n >= 2; k += 1, n /= 2) {
            path = (path << 1) | (n & 1);
        }

        max = &min_;
        while (k > 0) {
            if (path & 1) {
                max = &(*max)->right;
            } else {
                max = &(*max)->left;
            }
            path >>= 1;
            k -= 1;
        }

        count_ -= 1;

        child = *max;
        *max = nullptr;

        if (child == node) {
            if (child == min_) {
                min_ = nullptr;
            }
            return;
        }

        child->left = node->left;
        child->right = node->right;
        child->parent = node->parent;

        if (child->left) {
            child->left->parent = child;
        }

        if (child->right) {
            child->right->parent = child;
        }

        if (!node->parent) {
            min_ = child;
        } else if (node->parent->left == node) {
            node->parent->left = child;
        } else {
            node->parent->right = child;
        }

        for (;;) {
            smallest = child;
            if (child->left && compare_(owner_from_hook(child->left), owner_from_hook(smallest))) {
                smallest = child->left;
            }
            if (child->right && compare_(owner_from_hook(child->right), owner_from_hook(smallest))) {
                smallest = child->right;
            }
            if (smallest == child) {
                break;
            }
            swap_nodes(this, child, smallest);
        }

        while (child->parent && compare_(owner_from_hook(child), owner_from_hook(child->parent))) {
            swap_nodes(this, child->parent, child);
        }
    }

private:
    static void swap_nodes(BinaryHeap *heap, BinaryHeapNode *parent, BinaryHeapNode *child) noexcept {
        BinaryHeapNode *sibling;
        BinaryHeapNode temp = *parent;
        *parent = *child;
        *child = temp;

        parent->parent = child;
        if (child->left == child) {
            child->left = parent;
            sibling = child->right;
        } else {
            child->right = parent;
            sibling = child->left;
        }
        if (sibling) {
            sibling->parent = child;
        }

        if (parent->left) {
            parent->left->parent = parent;
        }
        if (parent->right) {
            parent->right->parent = parent;
        }

        if (!child->parent) {
            heap->min_ = child;
        } else if (child->parent->left == parent) {
            child->parent->left = child;
        } else {
            child->parent->right = child;
        }
    }

    [[nodiscard]] static BinaryHeapNode &hook_of(T &owner) noexcept {
        return *reinterpret_cast<BinaryHeapNode *>(reinterpret_cast<std::uint8_t *>(&owner) + Offset);
    }
    [[nodiscard]] static const BinaryHeapNode &hook_of(const T &owner) noexcept {
        return *reinterpret_cast<const BinaryHeapNode *>(reinterpret_cast<const std::uint8_t *>(&owner) + Offset);
    }
    [[nodiscard]] static T *owner_from_hook(BinaryHeapNode *hook) noexcept {
        return reinterpret_cast<T *>(reinterpret_cast<std::uint8_t *>(hook) - Offset);
    }
    [[nodiscard]] static const T *owner_from_hook(const BinaryHeapNode *hook) noexcept {
        return reinterpret_cast<const T *>(reinterpret_cast<const std::uint8_t *>(hook) - Offset);
    }

    BinaryHeapNode *min_ = nullptr;
    std::size_t count_ = 0;
    Compare compare_{};
};

} // namespace fiber::common

#endif // FIBER_COMMON_BINARY_HEAP_H
