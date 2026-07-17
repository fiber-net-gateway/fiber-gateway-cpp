#ifndef FIBER_NACOS_SUBSCRIPTION_POOL_H
#define FIBER_NACOS_SUBSCRIPTION_POOL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include <async/Watch.h>
#include <common/Assert.h>
#include <common/IntrusiveRbTree.h>
#include <fiber/nacos/Subscription.h>

#include "SubscriptionEntry.h"

namespace fiber::nacos::detail {

// Shared-subscription registry. Owns the RB tree of entries keyed by
// (data_id, group), the intrusive refcount lifecycle, and the Close fan-out.
// Deliberately free of any transport/loop/task concern: it only manages
// subscription state and fires two callbacks at add/remove time. The owner
// (e.g. ConfigServiceImpl) supplies the callbacks and drives connection-driven
// re-registration itself.
//
// Single-threaded contract: every method runs on the owner EventLoop. The
// public Subscription<T> returned to callers may be awaited from another loop;
// only its close()/destruction is owner-loop-only (enforced by the lease).
template<typename EntryT>
class SubscriptionPool {
public:
    using value_type = typename EntryT::value_type;
    using EntryPtrT = EntryPtr<EntryT>;
    using Result = typename EntryT::Result;
    using Snapshot = typename async::Watch<Result>::Snapshot;
    using Subscriber = typename async::Watch<Result>::Subscriber;

    // Called when an entry gains its first subscriber (ref_count goes 1->2).
    // Receives an owning EntryPtr so the owner can move it into an async
    // registration task without use-after-free.
    using AddCallback = std::function<void(EntryPtrT)>;
    // Called when an entry loses its last subscriber (ref_count back to 1, i.e.
    // only the tree holds it). Returns UnlinkNow to free immediately, or Defer
    // to keep the entry alive (unlinked from the tree) while an async
    // unregistration runs; the owning EntryPtr captured here releases it to 0.
    using RemoveCallback = std::function<RemoveDecision(EntryPtrT)>;

    SubscriptionPool(AddCallback on_add, RemoveCallback on_remove) noexcept :
        on_add_(std::move(on_add)), on_remove_(std::move(on_remove)) {}

    ~SubscriptionPool() {
        // A live pool should be disabled + close_all'd before destruction; but
        // be defensive: any still-linked entries are unlinked (released).
        while (auto *entry = entries_.minimum()) {
            unlink_entry(*entry);
        }
    }

    SubscriptionPool(const SubscriptionPool &) = delete;
    SubscriptionPool &operator=(const SubscriptionPool &) = delete;
    SubscriptionPool(SubscriptionPool &&) = delete;
    SubscriptionPool &operator=(SubscriptionPool &&) = delete;

    // Rejects new subscriptions. Called by the owner at shutdown.
    void disable() noexcept { active_ = false; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Subscribe to (id, group). Synchronous, non-coroutine: returns nullopt
    // when the pool is disabled. Key validation is the caller's job. On the
    // first subscriber for a key the AddCallback fires.
    [[nodiscard]] std::optional<::fiber::nacos::Subscription<value_type>> subscribe(std::string_view id,
                                                                                    std::string_view group) {
        FIBER_ASSERT(active_);
        EntryPtrT entry = find(id, group);
        const bool first = entry == nullptr;
        if (first) {
            entry = make_subscription_entry<EntryT>(id, group);
            entry->retain(); // tree's reference
            entries_.insert(*entry);
            entry->pool = this;
        }
        // subscriber reference (entry stays alive at least until lease close)
        entry->retain();
        // Stateless release trampoline erases EntryT so the public lease never
        // names it. Defined here (inside a member function) so it can reach the
        // pool's private release(); entry->pool is nulled once the entry leaves
        // the tree, making a dangling lease a no-op rather than a use-after-free.
        auto release_fn = +[](void *ctx) noexcept {
            auto *e = static_cast<EntryT *>(ctx);
            if (e->pool != nullptr) {
                e->pool->release(*e);
            }
        };
        typename ::fiber::nacos::Subscription<value_type>::Lease lease(entry.get(), release_fn);
        ::fiber::nacos::Subscription<value_type> subscription(std::move(lease), entry->watch.subscribe());
        if (first) {
            on_add_(EntryPtrT(entry.get())); // owning copy for the owner
        }
        return subscription;
    }

    // Publish a new value to an entry's watchers. No protocol logic here. The
    // owner wraps its protocol value into a Result (kind=Success) before calling.
    void publish(EntryT &entry, Result value) {
        FIBER_ASSERT(entry.publisher.has_value());
        entry.publisher->publish(std::move(value));
    }

    // Close every still-linked entry exactly once: publish Result{Closed} so
    // next() awaiters wake and see kind == Closed. Idempotent. Does not unlink
    // (the owner tears the tree down after). Single-threaded: close_all runs to
    // completion without interleaving other publishes, so there is no
    // revive-after-close race.
    void close_all() noexcept {
        if (closed_all_) {
            return;
        }
        closed_all_ = true;
        for (EntryT *entry = entries_.minimum(); entry != nullptr; entry = entries_.next_of(*entry)) {
            publish(*entry, Result{.kind = ResultKind::Closed, .data = std::nullopt});
        }
    }

    // Read-only access for the owner (register_all / push dispatch / queries).
    [[nodiscard]] EntryPtrT find(std::string_view id, std::string_view group) {
        const std::uint64_t hash = subscription_key_hash(id, group);
        EntryT *node = entries_.root();
        while (node != nullptr) {
            const int cmp = compare_entry_key(hash, id, group, node->key_hash, node->data_id, node->group);
            if (cmp == 0) {
                return EntryPtrT(node);
            }
            node = cmp < 0 ? entry_from_hook(node->tree_hook.left) : entry_from_hook(node->tree_hook.right);
        }
        return EntryPtrT{};
    }

    template<typename F>
    void for_each(F &&fn) {
        for (EntryT *entry = entries_.minimum(); entry != nullptr; entry = entries_.next_of(*entry)) {
            fn(*entry);
        }
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    using Registry = common::IntrusiveRbTree<EntryT, offsetof(EntryT, tree_hook), EntryKeyCompare<EntryT>>;

    // Lease->close path: drop one subscriber reference. When ref_count falls to
    // 1 (only the tree left), ask the owner whether to free now or defer. Defer
    // unlinks the entry from the tree (and nulls its pool pointer) but keeps it
    // alive via the owning EntryPtr the callback captured.
    void release(EntryT &entry) noexcept {
        const std::uint32_t prev = entry.ref_count.fetch_sub(1, std::memory_order_acq_rel);
        FIBER_ASSERT(prev >= 2); // at least tree (1) + this subscriber (1)
        if (prev != 2) {
            return; // other subscribers/tasks still hold it
        }
        // Only the tree holds it now.
        const RemoveDecision decision = on_remove_(EntryPtrT(&entry));
        if (decision == RemoveDecision::UnlinkNow) {
            unlink_entry(entry); // tree.erase + release tree ref -> 0 -> free
            return;
        }
        // Defer: detach from tree but keep alive. The owner's async path holds
        // the owning EntryPtr and will release it to 0 when done.
        if (entry.tree_hook.linked()) {
            entries_.erase(entry);
            entry.pool = nullptr;
            // drop the tree's reference; the callback's owning EntryPtr (and
            // any in-flight task) keeps ref_count >= 1 until they release.
            const std::uint32_t after = entry.ref_count.fetch_sub(1, std::memory_order_acq_rel);
            if (after == 1) {
                destroy_entry(&entry);
            }
        }
    }

    // Remove from tree and release the tree's reference (-> 0 frees).
    void unlink_entry(EntryT &entry) noexcept {
        if (entry.tree_hook.linked()) {
            entries_.erase(entry);
            entry.pool = nullptr;
        }
        const std::uint32_t prev = entry.ref_count.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            destroy_entry(&entry);
        }
    }

    [[nodiscard]] static EntryT *entry_from_hook(common::IntrusiveRbTreeHook *hook) noexcept {
        if (hook == nullptr || !hook->linked()) {
            return nullptr;
        }
        return reinterpret_cast<EntryT *>(reinterpret_cast<std::uint8_t *>(hook) - offsetof(EntryT, tree_hook));
    }

    AddCallback on_add_;
    RemoveCallback on_remove_;
    Registry entries_;
    bool active_ = true;
    bool closed_all_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_SUBSCRIPTION_POOL_H
