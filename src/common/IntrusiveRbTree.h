#ifndef FIBER_COMMON_INTRUSIVE_RB_TREE_H
#define FIBER_COMMON_INTRUSIVE_RB_TREE_H

#include <cstddef>
#include <cstdint>

#include "Assert.h"

namespace fiber::common {

enum class IntrusiveRbTreeColor : std::uint8_t {
    Black = 0,
    Red = 1,
};

struct IntrusiveRbTreeHook {
    IntrusiveRbTreeHook *left = nullptr;
    IntrusiveRbTreeHook *right = nullptr;
    IntrusiveRbTreeHook *parent = nullptr;
    IntrusiveRbTreeColor color = IntrusiveRbTreeColor::Black;
    bool in_tree = false;

    [[nodiscard]] bool linked() const noexcept { return in_tree; }
};

template<typename T, std::size_t Offset, typename Compare>
class IntrusiveRbTree {
public:
    IntrusiveRbTree() noexcept { init_sentinel(); }

    explicit IntrusiveRbTree(Compare compare) noexcept : compare_(compare) { init_sentinel(); }

    IntrusiveRbTree(const IntrusiveRbTree &) = delete;
    IntrusiveRbTree &operator=(const IntrusiveRbTree &) = delete;
    IntrusiveRbTree(IntrusiveRbTree &&) = delete;
    IntrusiveRbTree &operator=(IntrusiveRbTree &&) = delete;

    [[nodiscard]] bool empty() const noexcept { return root_ == sentinel(); }

    [[nodiscard]] T *root() noexcept { return owner_from_tree_hook(root_); }

    [[nodiscard]] const T *root() const noexcept { return owner_from_tree_hook(root_); }

    [[nodiscard]] T *minimum() noexcept {
        if (empty()) {
            return nullptr;
        }
        return owner_from_hook(min_hook(root_));
    }

    [[nodiscard]] const T *minimum() const noexcept {
        if (empty()) {
            return nullptr;
        }
        return owner_from_hook(min_hook(root_));
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

        IntrusiveRbTreeHook **root = &root_;
        IntrusiveRbTreeHook *sentinel_node = sentinel();

        if (*root == sentinel_node) {
            hook.parent = nullptr;
            hook.left = sentinel_node;
            hook.right = sentinel_node;
            black(&hook);
            hook.in_tree = true;
            *root = &hook;
            return;
        }

        insert_value(*root, &hook);

        IntrusiveRbTreeHook *node = &hook;
        while (node != *root && is_red(node->parent)) {
            if (node->parent == node->parent->parent->left) {
                IntrusiveRbTreeHook *temp = node->parent->parent->right;

                if (is_red(temp)) {
                    black(node->parent);
                    black(temp);
                    red(node->parent->parent);
                    node = node->parent->parent;
                } else {
                    if (node == node->parent->right) {
                        node = node->parent;
                        left_rotate(root, node);
                    }

                    black(node->parent);
                    red(node->parent->parent);
                    right_rotate(root, node->parent->parent);
                }
            } else {
                IntrusiveRbTreeHook *temp = node->parent->parent->left;

                if (is_red(temp)) {
                    black(node->parent);
                    black(temp);
                    red(node->parent->parent);
                    node = node->parent->parent;
                } else {
                    if (node == node->parent->left) {
                        node = node->parent;
                        right_rotate(root, node);
                    }

                    black(node->parent);
                    red(node->parent->parent);
                    left_rotate(root, node->parent->parent);
                }
            }
        }

        black(*root);
        (*root)->parent = nullptr;
    }

    void erase(T &owner) noexcept {
        IntrusiveRbTreeHook *node = &hook_of(owner);
        if (!node->in_tree) {
            return;
        }

        IntrusiveRbTreeHook **root = &root_;
        IntrusiveRbTreeHook *sentinel_node = sentinel();
        IntrusiveRbTreeHook *subst;
        IntrusiveRbTreeHook *temp;

        if (node->left == sentinel_node) {
            temp = node->right;
            subst = node;
        } else if (node->right == sentinel_node) {
            temp = node->left;
            subst = node;
        } else {
            subst = min_hook(node->right);
            temp = subst->right;
        }

        if (subst == *root) {
            *root = temp;
            black(temp);
            if (temp != sentinel_node) {
                temp->parent = nullptr;
            } else {
                sentinel_node->parent = nullptr;
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

        if (subst == node) {
            temp->parent = subst->parent;
        } else {
            if (subst->parent == node) {
                temp->parent = subst;
            } else {
                temp->parent = subst->parent;
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

        clear_hook(*node);

        if (subst_red) {
            if (*root != sentinel_node) {
                (*root)->parent = nullptr;
            }
            return;
        }

        while (temp != *root && is_black(temp)) {
            IntrusiveRbTreeHook *w;

            if (temp == temp->parent->left) {
                w = temp->parent->right;

                if (is_red(w)) {
                    black(w);
                    red(temp->parent);
                    left_rotate(root, temp->parent);
                    w = temp->parent->right;
                }

                if (is_black(w->left) && is_black(w->right)) {
                    red(w);
                    temp = temp->parent;
                } else {
                    if (is_black(w->right)) {
                        black(w->left);
                        red(w);
                        right_rotate(root, w);
                        w = temp->parent->right;
                    }

                    copy_color(w, temp->parent);
                    black(temp->parent);
                    black(w->right);
                    left_rotate(root, temp->parent);
                    temp = *root;
                }
            } else {
                w = temp->parent->left;

                if (is_red(w)) {
                    black(w);
                    red(temp->parent);
                    right_rotate(root, temp->parent);
                    w = temp->parent->left;
                }

                if (is_black(w->left) && is_black(w->right)) {
                    red(w);
                    temp = temp->parent;
                } else {
                    if (is_black(w->left)) {
                        black(w->right);
                        red(w);
                        left_rotate(root, w);
                        w = temp->parent->left;
                    }

                    copy_color(w, temp->parent);
                    black(temp->parent);
                    black(w->left);
                    right_rotate(root, temp->parent);
                    temp = *root;
                }
            }
        }

        black(temp);
        if (*root != sentinel_node) {
            (*root)->parent = nullptr;
        }
        black(sentinel_node);
    }

private:
    void init_sentinel() noexcept {
        sentinel_.left = &sentinel_;
        sentinel_.right = &sentinel_;
        sentinel_.parent = nullptr;
        sentinel_.color = IntrusiveRbTreeColor::Black;
        sentinel_.in_tree = false;
        root_ = &sentinel_;
    }

    [[nodiscard]] IntrusiveRbTreeHook *sentinel() noexcept { return &sentinel_; }

    [[nodiscard]] const IntrusiveRbTreeHook *sentinel() const noexcept { return &sentinel_; }

    [[nodiscard]] static bool is_red(const IntrusiveRbTreeHook *hook) noexcept {
        return hook->color == IntrusiveRbTreeColor::Red;
    }

    [[nodiscard]] static bool is_black(const IntrusiveRbTreeHook *hook) noexcept { return !is_red(hook); }

    static void red(IntrusiveRbTreeHook *hook) noexcept { hook->color = IntrusiveRbTreeColor::Red; }

    static void black(IntrusiveRbTreeHook *hook) noexcept { hook->color = IntrusiveRbTreeColor::Black; }

    static void copy_color(IntrusiveRbTreeHook *dst, const IntrusiveRbTreeHook *src) noexcept {
        dst->color = src->color;
    }

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
        red(node);
        node->in_tree = true;
    }

    static void clear_hook(IntrusiveRbTreeHook &hook) noexcept {
        hook.left = nullptr;
        hook.right = nullptr;
        hook.parent = nullptr;
        hook.color = IntrusiveRbTreeColor::Black;
        hook.in_tree = false;
    }

    void left_rotate(IntrusiveRbTreeHook **root, IntrusiveRbTreeHook *node) noexcept {
        IntrusiveRbTreeHook *temp = node->right;
        node->right = temp->left;

        if (temp->left != sentinel()) {
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

    void right_rotate(IntrusiveRbTreeHook **root, IntrusiveRbTreeHook *node) noexcept {
        IntrusiveRbTreeHook *temp = node->left;
        node->left = temp->right;

        if (temp->right != sentinel()) {
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

            if (node == root_) {
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

            if (node == root_) {
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
    IntrusiveRbTreeHook *root_ = &sentinel_;
    Compare compare_{};
};

} // namespace fiber::common

#endif // FIBER_COMMON_INTRUSIVE_RB_TREE_H
