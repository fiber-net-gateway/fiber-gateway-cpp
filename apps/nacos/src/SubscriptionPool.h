#ifndef FIBER_NACOS_SUBSCRIPTION_POOL_H
#define FIBER_NACOS_SUBSCRIPTION_POOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/common/IntrusiveList.h>
#include <fiber/common/IntrusiveRbTree.h>
#include <fiber/nacos/Subscription.h>

namespace fiber::nacos::detail {

// Decision returned when the last local callback leaves an entry. RetireNow
// removes it from the registry immediately. KeepLinked lets protocol code keep
// the same entry while an in-flight subscribe/unsubscribe converges; it calls
// SubscriptionPool::retire() once the entry is both idle and unregistered.
enum class RemoveDecision : std::uint8_t { RetireNow, KeepLinked };

// Shared-subscription registry keyed by (data_id, group). Each entry owns an
// intrusive list of callback nodes. List transitions, not the entry refcount,
// define logical subscription state: 0->1 calls on_add and 1->0 calls
// on_remove. The refcount is reserved for registry and in-flight task lifetime.
//
// ProtocolState keeps transport-specific reconciliation state in the same
// allocation as the generic callback state without making the pool depend on a
// caller-defined Entry layout.
//
// Single-threaded contract: every method, callback, Subscription close, and
// destruction runs on the owner EventLoop.
template<typename Value, typename ProtocolState>
class SubscriptionPool {
public:
    using value_type = Value;
    using Result = SubscriptionResult<Value>;
    using SubscriptionT = ::fiber::nacos::Subscription<Value>;
    using NotifyCallback = typename SubscriptionT::NotifyCallback;

    struct Entry;

private:
    // Stable-address callback registration owned by the public Subscription
    // handle. Subscription is movable, so the intrusive node is allocated
    // separately. entry is nulled when shutdown orphans a surviving handle.
    struct Node {
        common::IntrusiveListHook hook{};
        Entry *entry = nullptr;
        NotifyCallback callback = nullptr;
        void *ctx = nullptr;
        std::uint64_t subscribed_version = 0;
        std::uint64_t last_notified_version = 0;
        bool closed = false;
    };

    using SubscriberList = common::IntrusiveList<Node, offsetof(Node, hook)>;

public:
    // Per-(data_id, group) state. Key bytes are stored in the same malloc block
    // immediately after Entry. The registry and in-flight EntryPtr instances
    // own intrusive references; callback presence is represented only by the
    // subscribers list.
    struct Entry {
        SubscriptionPool *pool = nullptr;
        std::uint64_t key_hash = 0;
        std::string_view data_id;
        std::string_view group;
        std::shared_ptr<const Value> latest;
        std::uint64_t notify_version = 0;
        SubscriberList subscribers;
        Node **notify_next_ptr = nullptr;
        bool notifying = false;
        bool notify_pending = false;
        bool closing = false;
        common::IntrusiveRbTreeHook tree_hook{};
        std::atomic<std::uint32_t> ref_count{0};
        ProtocolState proto{};

        void retain() noexcept { ref_count.fetch_add(1, std::memory_order_relaxed); }
    };

    // Intrusive owning handle for Entry. Entry cannot use std::shared_ptr
    // without a separate control-block allocation because its key bytes trail
    // the Entry object in the same malloc block.
    class EntryPtr {
    public:
        EntryPtr() noexcept = default;

        explicit EntryPtr(Entry *entry) noexcept : entry_(entry) {
            if (entry_ != nullptr) {
                entry_->retain();
            }
        }

        EntryPtr(const EntryPtr &other) noexcept : entry_(other.entry_) {
            if (entry_ != nullptr) {
                entry_->retain();
            }
        }

        EntryPtr(EntryPtr &&other) noexcept : entry_(std::exchange(other.entry_, nullptr)) {}

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
                entry_ = std::exchange(other.entry_, nullptr);
            }
            return *this;
        }

        ~EntryPtr() { reset(); }

        [[nodiscard]] Entry &operator*() const noexcept { return *entry_; }
        [[nodiscard]] Entry *operator->() const noexcept { return entry_; }
        [[nodiscard]] Entry *get() const noexcept { return entry_; }
        [[nodiscard]] explicit operator bool() const noexcept { return entry_ != nullptr; }

        void reset() noexcept {
            if (entry_ != nullptr) {
                const std::uint32_t previous = entry_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
                if (previous == 1) {
                    destroy_entry(entry_);
                }
                entry_ = nullptr;
            }
        }

        friend bool operator==(const EntryPtr &left, std::nullptr_t) noexcept { return left.entry_ == nullptr; }
        friend bool operator==(std::nullptr_t, const EntryPtr &right) noexcept { return right.entry_ == nullptr; }

    private:
        Entry *entry_ = nullptr;
    };

    using AddCallback = std::function<void(EntryPtr)>;
    using RemoveCallback = std::function<RemoveDecision(EntryPtr)>;

    SubscriptionPool(AddCallback on_add, RemoveCallback on_remove) noexcept :
        on_add_(std::move(on_add)), on_remove_(std::move(on_remove)) {}

    ~SubscriptionPool() {
        active_ = false;
        closing_ = true;
        while (Entry *entry = entries_.minimum()) {
            EntryPtr hold(entry);
            erase_tree_reference(*entry);
            orphan_subscribers(*entry);
            entry->pool = nullptr;
        }
    }

    SubscriptionPool(const SubscriptionPool &) = delete;
    SubscriptionPool &operator=(const SubscriptionPool &) = delete;
    SubscriptionPool(SubscriptionPool &&) = delete;
    SubscriptionPool &operator=(SubscriptionPool &&) = delete;

    void disable() noexcept { active_ = false; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] std::optional<SubscriptionT> subscribe(std::string_view id, std::string_view group,
                                                         NotifyCallback callback, void *ctx) {
        if (!active_) {
            return std::nullopt;
        }
        FIBER_ASSERT(callback != nullptr);

        EntryPtr entry = find(id, group);
        if (entry == nullptr) {
            entry = make_entry(id, group);
            entry->retain(); // registry reference
            entries_.insert(*entry);
            entry->pool = this;
        }

        const bool first_subscriber = entry->subscribers.empty();
        Node *node = make_node();
        node->entry = entry.get();
        node->callback = callback;
        node->ctx = ctx;
        node->subscribed_version = entry->notify_version;
        entry->subscribers.push_back(*node);

        SubscriptionT subscription(node, &close_subscription, &subscription_closed);
        if (first_subscriber) {
            on_add_(EntryPtr(entry.get()));
        }

        // Replay an existing cached value to a newly added callback. If a
        // notification is already being dispatched, defer replay to its outer
        // loop so callback execution does not recurse.
        std::shared_ptr<const Value> latest = entry->latest;
        if (latest != nullptr && node->last_notified_version < entry->notify_version) {
            if (entry->notifying) {
                entry->notify_pending = true;
            } else {
                node->last_notified_version = entry->notify_version;
                NotifyCallback notify = node->callback;
                void *notify_ctx = node->ctx;
                const Result result{.kind = ResultKind::Success, .data = std::move(latest)};
                notify(notify_ctx, result);
            }
        }
        return subscription;
    }

    void publish(Entry &raw_entry, std::shared_ptr<const Value> value) {
        EntryPtr entry(&raw_entry);
        if (entry->closing) {
            return;
        }
        if (entry->subscribers.empty()) {
            return;
        }
        FIBER_ASSERT(value != nullptr);

        FIBER_ASSERT(entry->notify_version != std::numeric_limits<std::uint64_t>::max());
        entry->latest = std::move(value);
        ++entry->notify_version;
        if (entry->notifying) {
            entry->notify_pending = true;
            return;
        }

        entry->notifying = true;
        do {
            entry->notify_pending = false;
            const std::uint64_t version = entry->notify_version;
            if (entry->closing) {
                const Result result{.kind = ResultKind::Closed, .data = nullptr};
                notify_round(*entry, result, version);
            } else if (entry->latest != nullptr) {
                const Result result{.kind = ResultKind::Success, .data = entry->latest};
                notify_round(*entry, result, version);
            }
        } while (entry->notify_pending && !entry->subscribers.empty());
        entry->notifying = false;

        if (entry->closing) {
            orphan_subscribers(*entry);
            entry->pool = nullptr;
        }
    }

    // Reject new subscriptions and deliver Closed to every callback that is
    // still active. Entries are removed from the registry before user code is
    // invoked, so callbacks may close subscriptions for arbitrary keys without
    // invalidating registry iteration. Surviving handles are orphaned and free
    // their own nodes later.
    void close_all() noexcept {
        if (closed_all_) {
            return;
        }
        active_ = false;
        closing_ = true;
        closed_all_ = true;

        while (Entry *raw_entry = entries_.minimum()) {
            EntryPtr entry(raw_entry);
            erase_tree_reference(*entry);
            entry->closing = true;
            FIBER_ASSERT(entry->notify_version != std::numeric_limits<std::uint64_t>::max());
            ++entry->notify_version;
            if (entry->notifying) {
                entry->notify_pending = true;
                continue;
            }
            const Result result{.kind = ResultKind::Closed, .data = nullptr};
            entry->notifying = true;
            notify_round(*entry, result, entry->notify_version);
            entry->notifying = false;
            orphan_subscribers(*entry);
            entry->pool = nullptr;
        }
    }

    [[nodiscard]] EntryPtr find(std::string_view id, std::string_view group) {
        const std::uint64_t hash = key_hash(id, group);
        Entry *node = entries_.root();
        while (node != nullptr) {
            const int comparison = compare_key(hash, id, group, node->key_hash, node->data_id, node->group);
            if (comparison == 0) {
                return EntryPtr(node);
            }
            node = comparison < 0 ? entry_from_hook(node->tree_hook.left) : entry_from_hook(node->tree_hook.right);
        }
        return EntryPtr{};
    }

    template<typename Function>
    void for_each(Function &&function) {
        for (Entry *entry = entries_.minimum(); entry != nullptr; entry = entries_.next_of(*entry)) {
            function(*entry);
        }
    }

    // Called by protocol reconciliation after an idle entry is confirmed to no
    // longer need remote state. A local subscriber may have revived the entry
    // while unregistration was in flight, in which case retirement is ignored.
    void retire(Entry &entry) noexcept {
        if (entry.pool != this || !entry.tree_hook.linked() || !entry.subscribers.empty()) {
            return;
        }
        EntryPtr hold(&entry);
        erase_tree_reference(entry);
        entry.pool = nullptr;
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    struct EntryCompare {
        [[nodiscard]] bool operator()(const Entry *left, const Entry *right) const noexcept {
            return compare_key(left->key_hash, left->data_id, left->group, right->key_hash, right->data_id,
                               right->group) < 0;
        }
    };

    using Registry = common::IntrusiveRbTree<Entry, offsetof(Entry, tree_hook), EntryCompare>;

    [[nodiscard]] static Node *make_node() {
        void *memory = std::malloc(sizeof(Node));
        FIBER_ASSERT(memory != nullptr);
        return new (memory) Node();
    }

    static void destroy_node(Node *node) noexcept {
        FIBER_ASSERT(node != nullptr);
        node->~Node();
        std::free(node);
    }

    [[nodiscard]] static EntryPtr make_entry(std::string_view data_id, std::string_view group) {
        static_assert(alignof(Entry) <= alignof(std::max_align_t),
                      "Entry alignment exceeds malloc's guarantee; use aligned allocation");
        const std::size_t total = sizeof(Entry) + data_id.size() + group.size() + 2;
        void *memory = std::malloc(total);
        FIBER_ASSERT(memory != nullptr);
        auto *entry = new (memory) Entry();

        char *tail = reinterpret_cast<char *>(entry) + sizeof(Entry);
        if (!data_id.empty()) {
            std::memcpy(tail, data_id.data(), data_id.size());
        }
        tail[data_id.size()] = '\0';
        char *group_tail = tail + data_id.size() + 1;
        if (!group.empty()) {
            std::memcpy(group_tail, group.data(), group.size());
        }
        group_tail[group.size()] = '\0';

        entry->data_id = std::string_view(tail, data_id.size());
        entry->group = std::string_view(group_tail, group.size());
        entry->key_hash = key_hash(data_id, group);
        return EntryPtr(entry);
    }

    static void destroy_entry(Entry *entry) noexcept {
        FIBER_ASSERT(entry != nullptr);
        entry->~Entry();
        std::free(entry);
    }

    [[nodiscard]] static std::uint64_t key_hash(std::string_view data_id, std::string_view group) noexcept {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        constexpr std::uint64_t prime = 0x100000001b3ull;
        auto mix = [&](std::string_view text) {
            for (unsigned char character: text) {
                hash ^= character;
                hash *= prime;
            }
            hash ^= 0x2f;
            hash *= prime;
        };
        mix(data_id);
        mix(group);
        return hash;
    }

    [[nodiscard]] static int compare_key(std::uint64_t left_hash, std::string_view left_data_id,
                                         std::string_view left_group, std::uint64_t right_hash,
                                         std::string_view right_data_id, std::string_view right_group) noexcept {
        if (left_hash != right_hash) {
            return left_hash < right_hash ? -1 : 1;
        }
        if (const int comparison = left_data_id.compare(right_data_id); comparison != 0) {
            return comparison < 0 ? -1 : 1;
        }
        if (const int comparison = left_group.compare(right_group); comparison != 0) {
            return comparison < 0 ? -1 : 1;
        }
        return 0;
    }

    static void close_subscription(void *context) noexcept {
        auto *node = static_cast<Node *>(context);
        FIBER_ASSERT(node != nullptr);
        if (node->entry == nullptr) {
            destroy_node(node);
            return;
        }
        Entry *entry = node->entry;
        FIBER_ASSERT(entry->pool != nullptr);
        entry->pool->remove_node(*entry, *node);
    }

    [[nodiscard]] static bool subscription_closed(const void *context) noexcept {
        const auto *node = static_cast<const Node *>(context);
        FIBER_ASSERT(node != nullptr);
        return node->closed;
    }

    void notify_round(Entry &entry, const Result &result, std::uint64_t version) noexcept {
        FIBER_ASSERT(entry.notify_next_ptr == nullptr);
        Node *next = nullptr;
        entry.notify_next_ptr = &next;
        for (Node *current = entry.subscribers.front(); current != nullptr; current = next) {
            next = entry.subscribers.next_of(*current);
            if (current->subscribed_version > version || current->last_notified_version >= version) {
                continue;
            }
            current->last_notified_version = version;
            if (result.kind == ResultKind::Closed) {
                current->closed = true;
            }
            NotifyCallback notify = current->callback;
            void *ctx = current->ctx;
            notify(ctx, result);
            // current may have been closed and freed by its callback.
        }
        entry.notify_next_ptr = nullptr;
    }

    void remove_node(Entry &entry, Node &node) noexcept {
        FIBER_ASSERT(node.entry == &entry);
        FIBER_ASSERT(node.hook.linked());
        Node *successor = entry.subscribers.next_of(node);
        if (entry.notify_next_ptr != nullptr && *entry.notify_next_ptr == &node) {
            *entry.notify_next_ptr = successor;
        }
        entry.subscribers.erase(node);
        node.entry = nullptr;
        destroy_node(&node);

        if (!entry.subscribers.empty()) {
            return;
        }
        entry.latest.reset();
        if (closing_ || entry.closing) {
            return;
        }

        const RemoveDecision decision = on_remove_(EntryPtr(&entry));
        if (decision == RemoveDecision::RetireNow) {
            retire(entry);
        }
    }

    void orphan_subscribers(Entry &entry) noexcept {
        FIBER_ASSERT(!entry.notifying);
        FIBER_ASSERT(entry.notify_next_ptr == nullptr);
        while (Node *node = entry.subscribers.front()) {
            entry.subscribers.erase(*node);
            node->entry = nullptr;
            node->closed = true;
        }
        entry.latest.reset();
    }

    void erase_tree_reference(Entry &entry) noexcept {
        FIBER_ASSERT(entry.tree_hook.linked());
        entries_.erase(entry);
        const std::uint32_t previous = entry.ref_count.fetch_sub(1, std::memory_order_acq_rel);
        FIBER_ASSERT(previous >= 1);
        if (previous == 1) {
            destroy_entry(&entry);
        }
    }

    [[nodiscard]] static Entry *entry_from_hook(common::IntrusiveRbTreeHook *hook) noexcept {
        if (hook == nullptr || !hook->linked()) {
            return nullptr;
        }
        return reinterpret_cast<Entry *>(reinterpret_cast<std::uint8_t *>(hook) - offsetof(Entry, tree_hook));
    }

    AddCallback on_add_;
    RemoveCallback on_remove_;
    Registry entries_;
    bool active_ = true;
    bool closing_ = false;
    bool closed_all_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_SUBSCRIPTION_POOL_H
