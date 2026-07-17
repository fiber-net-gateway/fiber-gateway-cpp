#ifndef FIBER_NACOS_SUBSCRIPTION_ENTRY_H
#define FIBER_NACOS_SUBSCRIPTION_ENTRY_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include <async/Watch.h>
#include <common/Assert.h>
#include <common/IntrusiveRbTree.h>
#include <fiber/nacos/Subscription.h>

namespace fiber::nacos::detail {

template<typename EntryT>
class SubscriptionPool;

// Decision returned by SubscriptionPool's on_subscription_remove callback.
//   UnlinkNow - no in-flight work: the pool unlinks and frees the entry at once.
//   Defer     - in-flight work remains: the pool unlinks the entry from the tree
//               (and nulls its pool pointer) but keeps it alive via the owning
//               EntryPtr the callback captured; the async path releases it to 0.
enum class RemoveDecision : std::uint8_t { UnlinkNow, Defer };

// Per-(id, group) subscription state. The key bytes (data_id, group) are tailed
// onto the same malloc block as the entry and exposed as string_views, so no
// std::string allocation is needed. tree_hook links the entry into the pool's
// registry RB tree; ref_count keeps it alive across tree + subscribers + tasks.
//
// ref_count is an intrusive reference count: the registry tree holds one
// reference (retained on insert, released on erase). Each live Subscription and
// each in-flight task holding an EntryPtr adds one more. ref_count==1 means
// only the tree still owns it, i.e. it should be torn down.
//
// The Watch carries SubscriptionResult<T>, not bare T: that way end-of-
// subscription (Closed) is delivered as an ordinary published value, with no
// separate closed flag or custom awaiter. T stays free of lifecycle state.
//
// This is a plain (standard-layout) struct by design: IntrusiveRbTree requires
// standard-layout owners because it does container_of via offsetof(tree_hook).
// Protocol-specific fields live in ProtocolState; nothing here may grow a base
// class or virtuals.
//
// Single-threaded contract: all entry mutation happens on the owner EventLoop.
// ref_count is atomic only because it is manipulated via EntryPtr which may be
// destroyed on a different loop than the one that created it in pathological
// cases; the owner-loop code paths are single-threaded.
template<typename T, typename ProtocolState>
struct SubscriptionEntry {
    using value_type = T;
    using Result = SubscriptionResult<T>;

    // Owning pool (owner-loop pointer). Nulled when the entry leaves the tree so
    // a dangling lease becomes a no-op rather than a use-after-free.
    SubscriptionPool<SubscriptionEntry> *pool = nullptr;
    // (data_id, group) FNV-1a hash; the RB tree compares this first and falls
    // back to a precise key comparison only on collision.
    std::uint64_t key_hash = 0;
    std::string_view data_id; // points into the tail-malloc block
    std::string_view group;
    async::Watch<Result> watch; // no initial value; current() is null until first publish
    std::optional<typename async::Watch<Result>::Publisher> publisher;
    common::IntrusiveRbTreeHook tree_hook{};
    std::atomic<std::uint32_t> ref_count{0};
    ProtocolState proto{};

    SubscriptionEntry() {
        publisher = watch.acquire_publisher();
        FIBER_ASSERT(publisher.has_value());
    }

    void retain() noexcept { ref_count.fetch_add(1, std::memory_order_relaxed); }
};

// Destroys and frees an entry allocated by make_subscription_entry. Lives
// outside the entry because SubscriptionEntry does not know its own full type
// (ProtocolState-bearing EntryT) and thus cannot run its own destructor + free.
template<typename EntryT>
void destroy_entry(EntryT *entry) noexcept {
    FIBER_ASSERT(entry != nullptr);
    entry->~EntryT();
    std::free(entry);
}

// Intrusive reference-counted handle for an entry. The entry is allocated as a
// single malloc block with its data_id/group bytes tailed onto it, so it cannot
// use std::shared_ptr without a control-block indirection; the refcount lives in
// the entry itself.
template<typename EntryT>
class EntryPtr {
public:
    EntryPtr() noexcept = default;

    explicit EntryPtr(EntryT *entry) noexcept : entry_(entry) {
        if (entry_ != nullptr) {
            entry_->retain();
        }
    }

    EntryPtr(const EntryPtr &other) noexcept : entry_(other.entry_) {
        if (entry_ != nullptr) {
            entry_->retain();
        }
    }

    EntryPtr(EntryPtr &&other) noexcept : entry_(other.entry_) { other.entry_ = nullptr; }

    EntryPtr &operator=(const EntryPtr &other) noexcept {
        if (this != &other) {
            if (other.entry_ != nullptr) {
                other.entry_->retain();
            }
            reset();
            entry_ = other.entry_;
        }
        return *this;
    }

    EntryPtr &operator=(EntryPtr &&other) noexcept {
        if (this != &other) {
            reset();
            entry_ = other.entry_;
            other.entry_ = nullptr;
        }
        return *this;
    }

    ~EntryPtr() { reset(); }

    [[nodiscard]] EntryT &operator*() const noexcept { return *entry_; }
    [[nodiscard]] EntryT *operator->() const noexcept { return entry_; }
    [[nodiscard]] EntryT *get() const noexcept { return entry_; }
    [[nodiscard]] explicit operator bool() const noexcept { return entry_ != nullptr; }

    void reset() noexcept {
        if (entry_ != nullptr) {
            const std::uint32_t prev = entry_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                destroy_entry(entry_);
            }
            entry_ = nullptr;
        }
    }

    friend bool operator==(const EntryPtr &lhs, std::nullptr_t) noexcept { return lhs.entry_ == nullptr; }
    friend bool operator==(std::nullptr_t, const EntryPtr &rhs) noexcept { return rhs.entry_ == nullptr; }

private:
    EntryT *entry_ = nullptr;
};

// FNV-1a over data_id then group, mixed so (a,b) and (b,a) differ. Deterministic
// within a process and allocation-free. Used both at entry creation and for the
// lookup key, so a search never has to hash twice.
[[nodiscard]] inline std::uint64_t subscription_key_hash(std::string_view data_id, std::string_view group) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const std::uint64_t prime = 0x100000001b3ull;
    auto mix = [&](std::string_view s) {
        for (unsigned char c: s) {
            hash ^= c;
            hash *= prime;
        }
        hash ^= 0x2f; // separator so "" + "ab" != "ab" + ""
        hash *= prime;
    };
    mix(data_id);
    mix(group);
    return hash;
}

// Allocates an entry together with its data_id/group bytes in a single malloc
// block: [EntryT][data_id]['\0'][group]['\0']. The entry's string_views point
// into the tail. The returned EntryPtr owns the single initial reference; the
// pool adds a second reference on insert. pool and proto are left default; the
// caller (the pool) sets pool after insert.
template<typename EntryT>
[[nodiscard]] EntryPtr<EntryT> make_subscription_entry(std::string_view data_id, std::string_view group) {
    static_assert(alignof(EntryT) <= alignof(std::max_align_t),
                  "EntryT alignment exceeds malloc's guarantee; use aligned allocation");
    const std::size_t total = sizeof(EntryT) + data_id.size() + group.size() + 2;
    void *mem = std::malloc(total);
    FIBER_ASSERT(mem != nullptr);
    auto *entry = new (mem) EntryT();
    char *tail = reinterpret_cast<char *>(entry) + sizeof(EntryT);
    if (data_id.size() != 0) {
        std::memcpy(tail, data_id.data(), data_id.size());
    }
    tail[data_id.size()] = '\0';
    char *group_tail = tail + data_id.size() + 1;
    if (group.size() != 0) {
        std::memcpy(group_tail, group.data(), group.size());
    }
    group_tail[group.size()] = '\0';
    entry->data_id = std::string_view(tail, data_id.size());
    entry->group = std::string_view(group_tail, group.size());
    entry->key_hash = subscription_key_hash(data_id, group);
    return EntryPtr<EntryT>(entry);
}

// Lightweight lookup key: hash plus the precise (data_id, group). Avoids
// constructing a temporary entry just to walk the tree.
template<typename EntryT>
struct EntryKeyView {
    std::uint64_t key_hash = 0;
    std::string_view data_id;
    std::string_view group;
};

[[nodiscard]] inline int compare_entry_key(std::uint64_t a_hash, std::string_view a_data_id, std::string_view a_group,
                                           std::uint64_t b_hash, std::string_view b_data_id,
                                           std::string_view b_group) noexcept {
    if (a_hash != b_hash) {
        return a_hash < b_hash ? -1 : 1;
    }
    if (int cmp = a_data_id.compare(b_data_id)) {
        return cmp < 0 ? -1 : 1;
    }
    if (int cmp = a_group.compare(b_group)) {
        return cmp < 0 ? -1 : 1;
    }
    return 0;
}

// RB tree comparator: hash first, then precise key. Used by the pool's registry.
template<typename EntryT>
struct EntryKeyCompare {
    using EntryT_ = EntryT;
    [[nodiscard]] bool operator()(const EntryT *lhs, const EntryT *rhs) const noexcept {
        return compare_entry_key(lhs->key_hash, lhs->data_id, lhs->group, rhs->key_hash, rhs->data_id, rhs->group) < 0;
    }
};

// (The public SubscriptionLease<T> lives in Subscription.h and type-erases the
// entry via a void* context + release function pointer, so no detail lease type
// is needed here. SubscriptionPool::subscribe installs the trampoline.)

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_SUBSCRIPTION_ENTRY_H
