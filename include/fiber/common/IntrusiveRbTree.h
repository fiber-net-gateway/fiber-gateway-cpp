#ifndef FIBER_COMMON_INTRUSIVE_RB_TREE_H
#define FIBER_COMMON_INTRUSIVE_RB_TREE_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Assert.h"

namespace fiber::common {

enum class IntrusiveRbTreeColor : std::uint8_t {
    Black = 0,
    Red = 1,
};

// Linked nodes lead through parent pointers to the unlinked sentinel. The
// sentinel owns the root in parent; its left/right remain self-linked NIL leaves.
// This lets a node erase itself without an owner pointer or a comparator.
struct IntrusiveRbTreeHook {
    IntrusiveRbTreeHook() noexcept = default;
    ~IntrusiveRbTreeHook() { unlink_self(); }

    IntrusiveRbTreeHook(const IntrusiveRbTreeHook &) = delete;
    IntrusiveRbTreeHook &operator=(const IntrusiveRbTreeHook &) = delete;
    IntrusiveRbTreeHook(IntrusiveRbTreeHook &&) = delete;
    IntrusiveRbTreeHook &operator=(IntrusiveRbTreeHook &&) = delete;

    IntrusiveRbTreeHook *left = nullptr;
    IntrusiveRbTreeHook *right = nullptr;
    IntrusiveRbTreeHook *parent = nullptr;
    IntrusiveRbTreeColor color = IntrusiveRbTreeColor::Black;
    bool in_tree = false;

    [[nodiscard]] bool linked() const noexcept { return in_tree; }

    void unlink_self() noexcept {
        if (!linked()) {
            return;
        }
        IntrusiveRbTreeHook *sentinel_node = parent;
        while (sentinel_node->linked()) {
            sentinel_node = sentinel_node->parent;
        }
        unlink_from(sentinel_node);
    }

private:
    template<typename T, std::size_t Offset, typename Compare>
    friend class IntrusiveRbTree;

    [[nodiscard]] static bool is_red(const IntrusiveRbTreeHook *hook) noexcept {
        return hook->color == IntrusiveRbTreeColor::Red;
    }

    [[nodiscard]] static bool is_black(const IntrusiveRbTreeHook *hook) noexcept { return !is_red(hook); }

    static void red(IntrusiveRbTreeHook *hook) noexcept { hook->color = IntrusiveRbTreeColor::Red; }

    static void black(IntrusiveRbTreeHook *hook) noexcept { hook->color = IntrusiveRbTreeColor::Black; }

    static void copy_color(IntrusiveRbTreeHook *dst, const IntrusiveRbTreeHook *src) noexcept {
        dst->color = src->color;
    }

    static void clear_hook(IntrusiveRbTreeHook &hook) noexcept {
        hook.left = nullptr;
        hook.right = nullptr;
        hook.parent = nullptr;
        hook.color = IntrusiveRbTreeColor::Black;
        hook.in_tree = false;
    }

    static void left_rotate(IntrusiveRbTreeHook *sentinel_node, IntrusiveRbTreeHook *node) noexcept {
        IntrusiveRbTreeHook **root = &sentinel_node->parent;
        IntrusiveRbTreeHook *temp = node->right;
        node->right = temp->left;

        if (temp->left != sentinel_node) {
            temp->left->parent = node;
        }

        temp->parent = node->parent;

        if (node == *root) {
            *root = temp;
        } else if (node == node->parent->left) {
            node->parent->left = temp;
        } else {
            node->parent->right = temp;
        }

        temp->left = node;
        node->parent = temp;
    }

    static void right_rotate(IntrusiveRbTreeHook *sentinel_node, IntrusiveRbTreeHook *node) noexcept {
        IntrusiveRbTreeHook **root = &sentinel_node->parent;
        IntrusiveRbTreeHook *temp = node->left;
        node->left = temp->right;

        if (temp->right != sentinel_node) {
            temp->right->parent = node;
        }

        temp->parent = node->parent;

        if (node == *root) {
            *root = temp;
        } else if (node == node->parent->right) {
            node->parent->right = temp;
        } else {
            node->parent->left = temp;
        }

        temp->right = node;
        node->parent = temp;
    }

    void unlink_from(IntrusiveRbTreeHook *sentinel_node) noexcept {
        IntrusiveRbTreeHook *node = this;
        IntrusiveRbTreeHook **root = &sentinel_node->parent;
        IntrusiveRbTreeHook *subst;
        IntrusiveRbTreeHook *temp;

        if (node->left == sentinel_node) {
            temp = node->right;
            subst = node;
        } else if (node->right == sentinel_node) {
            temp = node->left;
            subst = node;
        } else {
            subst = node->right;
            while (subst->left != sentinel_node) {
                subst = subst->left;
            }
            temp = subst->right;
        }

        if (subst == *root) {
            *root = temp;
            black(temp);
            if (temp != sentinel_node) {
                temp->parent = sentinel_node;
            }
            clear_hook(*node);
            return;
        }

        const bool subst_red = is_red(subst);

        if (subst == subst->parent->left) {
            subst->parent->left = temp;
        } else {
            subst->parent->right = temp;
        }

        // The sentinel owns the root; never use its parent as deletion scratch space.
        IntrusiveRbTreeHook *temp_parent;
        if (subst == node) {
            temp_parent = subst->parent;
        } else {
            if (subst->parent == node) {
                temp_parent = subst;
            } else {
                temp_parent = subst->parent;
            }

            subst->left = node->left;
            subst->right = node->right;
            subst->parent = node->parent;
            copy_color(subst, node);

            if (node == *root) {
                *root = subst;
            } else if (node == node->parent->left) {
                node->parent->left = subst;
            } else {
                node->parent->right = subst;
            }

            if (subst->left != sentinel_node) {
                subst->left->parent = subst;
            }

            if (subst->right != sentinel_node) {
                subst->right->parent = subst;
            }
        }

        if (temp != sentinel_node) {
            temp->parent = temp_parent;
        }
        clear_hook(*node);

        if (subst_red) {
            return;
        }

        while (temp != *root && is_black(temp)) {
            IntrusiveRbTreeHook *w;

            if (temp == temp_parent->left) {
                w = temp_parent->right;

                if (is_red(w)) {
                    black(w);
                    red(temp_parent);
                    left_rotate(sentinel_node, temp_parent);
                    w = temp_parent->right;
                }

                if (is_black(w->left) && is_black(w->right)) {
                    red(w);
                    temp = temp_parent;
                    temp_parent = temp->parent;
                } else {
                    if (is_black(w->right)) {
                        black(w->left);
                        red(w);
                        right_rotate(sentinel_node, w);
                        w = temp_parent->right;
                    }

                    copy_color(w, temp_parent);
                    black(temp_parent);
                    black(w->right);
                    left_rotate(sentinel_node, temp_parent);
                    temp = *root;
                }
            } else {
                w = temp_parent->left;

                if (is_red(w)) {
                    black(w);
                    red(temp_parent);
                    right_rotate(sentinel_node, temp_parent);
                    w = temp_parent->left;
                }

                if (is_black(w->left) && is_black(w->right)) {
                    red(w);
                    temp = temp_parent;
                    temp_parent = temp->parent;
                } else {
                    if (is_black(w->left)) {
                        black(w->right);
                        red(w);
                        left_rotate(sentinel_node, w);
                        w = temp_parent->left;
                    }

                    copy_color(w, temp_parent);
                    black(temp_parent);
                    black(w->left);
                    right_rotate(sentinel_node, temp_parent);
                    temp = *root;
                }
            }
        }

        black(temp);
        black(sentinel_node);
    }
};

template<typename T, std::size_t Offset, typename Compare>
class IntrusiveRbTree {
    // Non-polymorphic rather than standard-layout, for the reasons spelled out on
    // IntrusiveList's identical assert in IntrusiveList.h.
    static_assert(!std::is_polymorphic_v<T>,
                  "IntrusiveRbTree owner type must be non-polymorphic because Offset is used for container_of.");

public:
    IntrusiveRbTree() noexcept { init_sentinel(); }

    explicit IntrusiveRbTree(Compare compare) noexcept : compare_(compare) { init_sentinel(); }

    ~IntrusiveRbTree() {
        // Nodes must unlink before their sentinel is destroyed.
        FIBER_ASSERT(empty());
    }

    IntrusiveRbTree(const IntrusiveRbTree &) = delete;
    IntrusiveRbTree &operator=(const IntrusiveRbTree &) = delete;
    IntrusiveRbTree(IntrusiveRbTree &&) = delete;
    IntrusiveRbTree &operator=(IntrusiveRbTree &&) = delete;

    [[nodiscard]] bool empty() const noexcept { return sentinel_.parent == sentinel(); }

    [[nodiscard]] T *root() noexcept { return owner_from_tree_hook(sentinel_.parent); }

    [[nodiscard]] const T *root() const noexcept { return owner_from_tree_hook(sentinel_.parent); }

    [[nodiscard]] T *minimum() noexcept {
        if (empty()) {
            return nullptr;
        }
        return owner_from_hook(min_hook(sentinel_.parent));
    }

    [[nodiscard]] const T *minimum() const noexcept {
        if (empty()) {
            return nullptr;
        }
        return owner_from_hook(min_hook(sentinel_.parent));
    }

    [[nodiscard]] T *next_of(T &owner) noexcept {
        IntrusiveRbTreeHook &hook = hook_of(owner);
        if (!hook.in_tree) {
            return nullptr;
        }
        return owner_from_tree_hook(next_hook(&hook));
    }

    [[nodiscard]] const T *next_of(const T &owner) const noexcept {
        const IntrusiveRbTreeHook &hook = hook_of(owner);
        if (!hook.in_tree) {
            return nullptr;
        }
        return owner_from_tree_hook(next_hook(&hook));
    }

    void insert(T &owner) noexcept {
        IntrusiveRbTreeHook &hook = hook_of(owner);
        FIBER_ASSERT(!hook.in_tree);

        IntrusiveRbTreeHook **root = &sentinel_.parent;
        IntrusiveRbTreeHook *sentinel_node = sentinel();

        if (*root == sentinel_node) {
            hook.parent = sentinel_node;
            hook.left = sentinel_node;
            hook.right = sentinel_node;
            IntrusiveRbTreeHook::black(&hook);
            hook.in_tree = true;
            *root = &hook;
            return;
        }

        insert_value(*root, &hook);

        IntrusiveRbTreeHook *node = &hook;
        while (node != *root && IntrusiveRbTreeHook::is_red(node->parent)) {
            if (node->parent == node->parent->parent->left) {
                IntrusiveRbTreeHook *temp = node->parent->parent->right;

                if (IntrusiveRbTreeHook::is_red(temp)) {
                    IntrusiveRbTreeHook::black(node->parent);
                    IntrusiveRbTreeHook::black(temp);
                    IntrusiveRbTreeHook::red(node->parent->parent);
                    node = node->parent->parent;
                } else {
                    if (node == node->parent->right) {
                        node = node->parent;
                        IntrusiveRbTreeHook::left_rotate(sentinel_node, node);
                    }

                    IntrusiveRbTreeHook::black(node->parent);
                    IntrusiveRbTreeHook::red(node->parent->parent);
                    IntrusiveRbTreeHook::right_rotate(sentinel_node, node->parent->parent);
                }
            } else {
                IntrusiveRbTreeHook *temp = node->parent->parent->left;

                if (IntrusiveRbTreeHook::is_red(temp)) {
                    IntrusiveRbTreeHook::black(node->parent);
                    IntrusiveRbTreeHook::black(temp);
                    IntrusiveRbTreeHook::red(node->parent->parent);
                    node = node->parent->parent;
                } else {
                    if (node == node->parent->left) {
                        node = node->parent;
                        IntrusiveRbTreeHook::right_rotate(sentinel_node, node);
                    }

                    IntrusiveRbTreeHook::black(node->parent);
                    IntrusiveRbTreeHook::red(node->parent->parent);
                    IntrusiveRbTreeHook::left_rotate(sentinel_node, node->parent->parent);
                }
            }
        }

        IntrusiveRbTreeHook::black(*root);
        (*root)->parent = sentinel_node;
    }

    void erase(T &owner) noexcept {
        IntrusiveRbTreeHook &hook = hook_of(owner);
        if (hook.linked()) {
            hook.unlink_from(sentinel());
        }
    }

private:
    void init_sentinel() noexcept {
        sentinel_.left = &sentinel_;
        sentinel_.right = &sentinel_;
        sentinel_.parent = &sentinel_;
        sentinel_.color = IntrusiveRbTreeColor::Black;
        sentinel_.in_tree = false;
    }

    [[nodiscard]] IntrusiveRbTreeHook *sentinel() noexcept { return &sentinel_; }

    [[nodiscard]] const IntrusiveRbTreeHook *sentinel() const noexcept { return &sentinel_; }

    void insert_value(IntrusiveRbTreeHook *temp, IntrusiveRbTreeHook *node) noexcept {
        IntrusiveRbTreeHook **slot;

        for (;;) {
            slot = compare_(owner_from_hook(node), owner_from_hook(temp)) ? &temp->left : &temp->right;

            if (*slot == sentinel()) {
                break;
            }

            temp = *slot;
        }

        *slot = node;
        node->parent = temp;
        node->left = sentinel();
        node->right = sentinel();
        IntrusiveRbTreeHook::red(node);
        node->in_tree = true;
    }

    [[nodiscard]] IntrusiveRbTreeHook *min_hook(IntrusiveRbTreeHook *node) noexcept {
        while (node->left != sentinel()) {
            node = node->left;
        }
        return node;
    }

    [[nodiscard]] const IntrusiveRbTreeHook *min_hook(const IntrusiveRbTreeHook *node) const noexcept {
        while (node->left != sentinel()) {
            node = node->left;
        }
        return node;
    }

    [[nodiscard]] IntrusiveRbTreeHook *next_hook(IntrusiveRbTreeHook *node) noexcept {
        if (node->right != sentinel()) {
            return min_hook(node->right);
        }

        for (;;) {
            IntrusiveRbTreeHook *parent = node->parent;

            if (node == sentinel_.parent) {
                return sentinel();
            }

            if (node == parent->left) {
                return parent;
            }

            node = parent;
        }
    }

    [[nodiscard]] const IntrusiveRbTreeHook *next_hook(const IntrusiveRbTreeHook *node) const noexcept {
        if (node->right != sentinel()) {
            return min_hook(node->right);
        }

        for (;;) {
            const IntrusiveRbTreeHook *parent = node->parent;

            if (node == sentinel_.parent) {
                return sentinel();
            }

            if (node == parent->left) {
                return parent;
            }

            node = parent;
        }
    }

    [[nodiscard]] static IntrusiveRbTreeHook &hook_of(T &owner) noexcept {
        return *reinterpret_cast<IntrusiveRbTreeHook *>(reinterpret_cast<std::uint8_t *>(&owner) + Offset);
    }

    [[nodiscard]] static const IntrusiveRbTreeHook &hook_of(const T &owner) noexcept {
        return *reinterpret_cast<const IntrusiveRbTreeHook *>(reinterpret_cast<const std::uint8_t *>(&owner) + Offset);
    }

    [[nodiscard]] static T *owner_from_hook(IntrusiveRbTreeHook *hook) noexcept {
        return reinterpret_cast<T *>(reinterpret_cast<std::uint8_t *>(hook) - Offset);
    }

    [[nodiscard]] static const T *owner_from_hook(const IntrusiveRbTreeHook *hook) noexcept {
        return reinterpret_cast<const T *>(reinterpret_cast<const std::uint8_t *>(hook) - Offset);
    }

    [[nodiscard]] T *owner_from_tree_hook(IntrusiveRbTreeHook *hook) noexcept {
        return hook == sentinel() ? nullptr : owner_from_hook(hook);
    }

    [[nodiscard]] const T *owner_from_tree_hook(const IntrusiveRbTreeHook *hook) const noexcept {
        return hook == sentinel() ? nullptr : owner_from_hook(hook);
    }

    IntrusiveRbTreeHook sentinel_{};
    Compare compare_{};
};

} // namespace fiber::common

#endif // FIBER_COMMON_INTRUSIVE_RB_TREE_H
