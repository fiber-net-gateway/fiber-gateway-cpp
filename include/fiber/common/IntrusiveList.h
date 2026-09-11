#ifndef FIBER_COMMON_INTRUSIVE_LIST_H
#define FIBER_COMMON_INTRUSIVE_LIST_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Assert.h"

namespace fiber::common {

// Ring hook: an unlinked hook is self-linked (prev == next == this); a linked
// hook sits in a circular ring whose anchor is the IntrusiveList itself, so
// every prev/next is non-null while linked. That is what lets the hook unlink
// itself -- from the destructor, or unlink_self() -- without knowing its list.
// Ring only: a hook driven by a hand-rolled null-terminated queue has null
// prev/next at the ends and must not reach unlink_self() (or be destroyed
// while queued) -- unlink would dereference null.
struct IntrusiveListHook {
    IntrusiveListHook() noexcept = default;
    ~IntrusiveListHook() { unlink_self(); }

    IntrusiveListHook(const IntrusiveListHook &) = delete;
    IntrusiveListHook &operator=(const IntrusiveListHook &) = delete;
    IntrusiveListHook(IntrusiveListHook &&) = delete;
    IntrusiveListHook &operator=(IntrusiveListHook &&) = delete;

    [[nodiscard]] bool linked() const noexcept { return next != this; }

    void unlink_self() noexcept {
        prev->next = next;
        next->prev = prev;
        prev = this;
        next = this;
    }

    // Join the ring that `position` belongs to, immediately before/after it.
    // The hook must be unlinked; `position` may be any ring member including
    // an anchor. Together with unlink_self() these cover every ring mutation,
    // so containers whose owner type cannot be named where they are declared
    // can still drive hooks without raw pointer writes.
    void insert_before(IntrusiveListHook &position) noexcept {
        FIBER_ASSERT(!linked());
        prev = position.prev;
        next = &position;
        position.prev->next = this;
        position.prev = this;
    }

    void insert_after(IntrusiveListHook &position) noexcept {
        FIBER_ASSERT(!linked());
        prev = &position;
        next = position.next;
        position.next->prev = this;
        position.next = this;
    }

    IntrusiveListHook *prev = this;
    IntrusiveListHook *next = this;
};

template<typename T, std::size_t Offset>
class IntrusiveList {
    // Non-polymorphic rather than standard-layout: `offsetof` on a non-standard-layout
    // type is conditionally-supported since C++17 and both clang and GCC support it.
    // The one case they cannot compute -- a member reached through a virtual base --
    // is a hard compile error, not a warning, so it cannot slip past this assert even
    // though std::is_polymorphic_v is false for a class with a virtual base and no
    // virtual functions. Sites that rely on this get a -Winvalid-offsetof warning at
    // the offsetof itself and silence it locally, which keeps them easy to find.
    static_assert(!std::is_polymorphic_v<T>,
                  "IntrusiveList owner type must be non-polymorphic because Offset is used for container_of.");

public:
    IntrusiveList() noexcept = default;
    ~IntrusiveList() {
        // A linked node's prev/next reach into this list: if the list died
        // first, the node's own unlink (destructor or unlink_self) would write
        // through a dead anchor. Owners must drain or unlink their nodes
        // before the list goes away.
        FIBER_ASSERT(empty());
    }

    IntrusiveList(const IntrusiveList &) = delete;
    IntrusiveList &operator=(const IntrusiveList &) = delete;
    IntrusiveList(IntrusiveList &&) = delete;
    IntrusiveList &operator=(IntrusiveList &&) = delete;

    [[nodiscard]] bool empty() const noexcept { return anchor_.next == &anchor_; }

    // front()/back() and next_of()/prev_of() keep the null-terminated contract
    // of the old head/tail list: they return nullptr at the ring ends (and for
    // an unlinked node), so `for (T *n = l.front(); n; n = l.next_of(*n))`
    // iterates exactly the linked nodes and never hands out the anchor.
    [[nodiscard]] T *front() noexcept { return node_or_null(anchor_.next); }
    [[nodiscard]] const T *front() const noexcept { return node_or_null(anchor_.next); }
    [[nodiscard]] T *back() noexcept { return node_or_null(anchor_.prev); }
    [[nodiscard]] const T *back() const noexcept { return node_or_null(anchor_.prev); }

    [[nodiscard]] T *next_of(T &owner) noexcept {
        IntrusiveListHook &hook = hook_of(owner);
        if (!hook.linked() || hook.next == &anchor_) {
            return nullptr;
        }
        return owner_from_hook(hook.next);
    }
    [[nodiscard]] const T *next_of(const T &owner) const noexcept {
        const IntrusiveListHook &hook = hook_of(owner);
        if (!hook.linked() || hook.next == &anchor_) {
            return nullptr;
        }
        return owner_from_hook(hook.next);
    }
    [[nodiscard]] T *prev_of(T &owner) noexcept {
        IntrusiveListHook &hook = hook_of(owner);
        if (!hook.linked() || hook.prev == &anchor_) {
            return nullptr;
        }
        return owner_from_hook(hook.prev);
    }
    [[nodiscard]] const T *prev_of(const T &owner) const noexcept {
        const IntrusiveListHook &hook = hook_of(owner);
        if (!hook.linked() || hook.prev == &anchor_) {
            return nullptr;
        }
        return owner_from_hook(hook.prev);
    }

    void push_back(T &owner) noexcept {
        IntrusiveListHook &hook = hook_of(owner);
        if (hook.linked()) {
            return;
        }

        hook.insert_before(anchor_);
    }

    void push_front(T &owner) noexcept {
        IntrusiveListHook &hook = hook_of(owner);
        if (hook.linked()) {
            return;
        }

        hook.insert_after(anchor_);
    }

    void insert_after(T &position, T &owner) noexcept {
        IntrusiveListHook &pos_hook = hook_of(position);
        IntrusiveListHook &hook = hook_of(owner);
        if (!pos_hook.linked() || hook.linked()) {
            return;
        }

        hook.insert_after(pos_hook);
    }

    void erase(T &owner) noexcept {
        IntrusiveListHook &hook = hook_of(owner);
        if (!hook.linked()) {
            return;
        }

        hook.unlink_self();
    }

private:
    [[nodiscard]] static IntrusiveListHook &hook_of(T &owner) noexcept {
        return *reinterpret_cast<IntrusiveListHook *>(reinterpret_cast<std::uint8_t *>(&owner) + Offset);
    }

    [[nodiscard]] static const IntrusiveListHook &hook_of(const T &owner) noexcept {
        return *reinterpret_cast<const IntrusiveListHook *>(reinterpret_cast<const std::uint8_t *>(&owner) + Offset);
    }

    [[nodiscard]] static T *owner_from_hook(IntrusiveListHook *hook) noexcept {
        if (!hook) {
            return nullptr;
        }
        return reinterpret_cast<T *>(reinterpret_cast<std::uint8_t *>(hook) - Offset);
    }

    [[nodiscard]] static const T *owner_from_hook(const IntrusiveListHook *hook) noexcept {
        if (!hook) {
            return nullptr;
        }
        return reinterpret_cast<const T *>(reinterpret_cast<const std::uint8_t *>(hook) - Offset);
    }

    [[nodiscard]] T *node_or_null(IntrusiveListHook *hook) noexcept {
        return hook == &anchor_ ? nullptr : owner_from_hook(hook);
    }

    [[nodiscard]] const T *node_or_null(const IntrusiveListHook *hook) const noexcept {
        return hook == &anchor_ ? nullptr : owner_from_hook(hook);
    }

    IntrusiveListHook anchor_{};
};

} // namespace fiber::common

#endif // FIBER_COMMON_INTRUSIVE_LIST_H
