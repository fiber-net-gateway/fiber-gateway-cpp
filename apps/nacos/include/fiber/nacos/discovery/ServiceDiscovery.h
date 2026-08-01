#ifndef FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H
#define FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#include <async/Task.h>
#include <common/Assert.h>
#include <common/IntrusiveRbTree.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::nacos {

struct ServiceKeyView {
    std::string_view service_name;
    std::string_view group;
};

enum class ServiceReadyError : std::uint8_t {
    Closed,
    Retired,
    Shutdown,
};

enum class ServiceChangeKind : std::uint8_t {
    Initial,
    Update,
};

enum class ServiceRetireReason : std::uint8_t {
    Released,
    SubscriptionClosed,
    Shutdown,
};

// Owner-EventLoop-only registry of shared per-service derived state. StateOps
// owns snapshot interpretation while this class owns subscription deduplication,
// first-notification synchronization, and entry lifetime.
template<typename StateOps>
class ServiceDiscovery final : public common::NonCopyable, public common::NonMovable {
public:
    using State = typename StateOps::State;
    using StatePtr = typename StateOps::StatePtr;

    class ReadyAwaiter;
    class Lease;

private:
    enum class EntryPhase : std::uint8_t {
        Pending,
        Ready,
        Closed,
        Retired,
    };

    struct ReadyWaiter {
        ReadyWaiter *previous = nullptr;
        ReadyWaiter *next = nullptr;
        ReadyAwaiter *awaiter = nullptr;
        std::coroutine_handle<> coroutine{};
        bool linked = false;
    };

    struct Entry {
        ServiceDiscovery *owner = nullptr;
        std::uint64_t key_hash = 0;
        std::string_view service_name;
        std::string_view group;
        Subscription<ServiceInfo> subscription;
        StatePtr state;
        ReadyWaiter *waiters_head = nullptr;
        ReadyWaiter *waiters_tail = nullptr;
        std::uint32_t ref_count = 0;
        std::uint32_t lease_count = 0;
        EntryPhase phase = EntryPhase::Pending;
        ServiceReadyError ready_error = ServiceReadyError::Closed;
        bool state_retired = false;
        common::IntrusiveRbTreeHook tree_hook{};

        void retain() noexcept {
            FIBER_ASSERT(ref_count != std::numeric_limits<std::uint32_t>::max());
            ++ref_count;
        }
    };

    class EntryPtr final {
    public:
        EntryPtr() noexcept = default;
        explicit EntryPtr(Entry *entry) noexcept : entry_(entry) {
            if (entry_ != nullptr) {
                entry_->retain();
            }
        }
        EntryPtr(const EntryPtr &other) noexcept : EntryPtr(other.entry_) {}
        EntryPtr(EntryPtr &&other) noexcept : entry_(std::exchange(other.entry_, nullptr)) {}
        ~EntryPtr() { reset(); }

        EntryPtr &operator=(const EntryPtr &other) noexcept {
            if (this != &other) {
                EntryPtr copy(other);
                swap(copy);
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

        [[nodiscard]] Entry &operator*() const noexcept { return *entry_; }
        [[nodiscard]] Entry *operator->() const noexcept { return entry_; }
        [[nodiscard]] Entry *get() const noexcept { return entry_; }
        [[nodiscard]] explicit operator bool() const noexcept { return entry_ != nullptr; }

        friend bool operator==(const EntryPtr &left, std::nullptr_t) noexcept { return left.entry_ == nullptr; }
        friend bool operator==(std::nullptr_t, const EntryPtr &right) noexcept { return right.entry_ == nullptr; }

        void reset() noexcept {
            if (entry_ == nullptr) {
                return;
            }
            Entry *entry = std::exchange(entry_, nullptr);
            FIBER_ASSERT(entry->ref_count > 0);
            if (--entry->ref_count == 0) {
                ServiceDiscovery::destroy_entry(entry);
            }
        }

    private:
        void swap(EntryPtr &other) noexcept { std::swap(entry_, other.entry_); }

        Entry *entry_ = nullptr;
    };

    struct EntryCompare {
        [[nodiscard]] bool operator()(const Entry *left, const Entry *right) const noexcept {
            return compare_key(left->key_hash, left->service_name, left->group, right->key_hash, right->service_name,
                               right->group) < 0;
        }
    };

    using Registry = common::IntrusiveRbTree<Entry, offsetof(Entry, tree_hook), EntryCompare>;

public:
    class ReadyAwaiter final {
    public:
        ReadyAwaiter(const ReadyAwaiter &) = delete;
        ReadyAwaiter &operator=(const ReadyAwaiter &) = delete;
        ReadyAwaiter(ReadyAwaiter &&) = delete;
        ReadyAwaiter &operator=(ReadyAwaiter &&) = delete;

        ~ReadyAwaiter() {
            if (waiter_.linked) {
                FIBER_ASSERT(entry_ != nullptr);
                entry_->owner->remove_waiter(*entry_, waiter_);
            }
        }

        [[nodiscard]] bool await_ready() noexcept {
            FIBER_ASSERT(entry_ != nullptr);
            completed_ = entry_->phase != EntryPhase::Pending;
            return completed_;
        }

        bool await_suspend(std::coroutine_handle<> coroutine) noexcept {
            FIBER_ASSERT(entry_ != nullptr);
            FIBER_ASSERT(entry_->owner->loop_->in_loop());
            if (entry_->phase != EntryPhase::Pending) {
                completed_ = true;
                return false;
            }
            waiter_.coroutine = coroutine;
            waiter_.awaiter = this;
            entry_->owner->add_waiter(*entry_, waiter_);
            return true;
        }

        [[nodiscard]] std::expected<StatePtr, ServiceReadyError> await_resume() noexcept {
            FIBER_ASSERT(completed_ || entry_->phase != EntryPhase::Pending);
            completed_ = true;
            if (entry_->state != nullptr) {
                StatePtr state = entry_->state;
                entry_.reset();
                return state;
            }
            const ServiceReadyError error = entry_->ready_error;
            entry_.reset();
            return std::unexpected(error);
        }

    private:
        friend class ServiceDiscovery;

        explicit ReadyAwaiter(EntryPtr entry) noexcept : entry_(std::move(entry)) { FIBER_ASSERT(entry_ != nullptr); }

        EntryPtr entry_;
        ReadyWaiter waiter_{};
        bool completed_ = false;
    };

    class Lease final {
    public:
        Lease() noexcept = default;
        ~Lease() { reset(); }

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept : entry_(std::move(other.entry_)) {}

        Lease &operator=(Lease &&other) noexcept {
            if (this != &other) {
                reset();
                entry_ = std::move(other.entry_);
            }
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return entry_ != nullptr; }

        [[nodiscard]] StatePtr try_state() const noexcept {
            FIBER_ASSERT(entry_ != nullptr);
            return entry_->state;
        }

        [[nodiscard]] State &state() const noexcept {
            StatePtr current = try_state();
            FIBER_ASSERT(current != nullptr);
            return *current;
        }

        [[nodiscard]] StatePtr shared_state() const noexcept { return try_state(); }

        [[nodiscard]] ReadyAwaiter wait_ready() const noexcept {
            FIBER_ASSERT(entry_ != nullptr);
            return ReadyAwaiter(EntryPtr(entry_.get()));
        }

        [[nodiscard]] std::string_view service_name() const noexcept {
            FIBER_ASSERT(entry_ != nullptr);
            return entry_->service_name;
        }

        [[nodiscard]] std::string_view group() const noexcept {
            FIBER_ASSERT(entry_ != nullptr);
            return entry_->group;
        }

        void reset() noexcept {
            if (entry_ == nullptr) {
                return;
            }
            EntryPtr entry = std::move(entry_);
            FIBER_ASSERT(entry->owner != nullptr);
            entry->owner->release_lease(*entry);
        }

    private:
        friend class ServiceDiscovery;

        explicit Lease(EntryPtr entry) noexcept : entry_(std::move(entry)) { FIBER_ASSERT(entry_ != nullptr); }

        EntryPtr entry_;
    };

    ServiceDiscovery(event::EventLoop &loop, NamingService &naming_service, StateOps ops = {}) noexcept :
        loop_(&loop), naming_service_(&naming_service), ops_(std::move(ops)) {}

    ~ServiceDiscovery() {
        FIBER_ASSERT(entries_.empty());
        FIBER_ASSERT(live_entries_ == 0);
    }

    [[nodiscard]] std::expected<Lease, NamingServiceError> acquire(std::string_view service_name,
                                                                   std::string_view group) {
        FIBER_ASSERT(loop_->in_loop());
        if (stopping_) {
            return std::unexpected(stopping_error());
        }

        EntryPtr entry = find(service_name, group);
        if (entry != nullptr) {
            FIBER_ASSERT(entry->lease_count != std::numeric_limits<std::uint32_t>::max());
            ++entry->lease_count;
            return Lease(std::move(entry));
        }

        entry = make_entry(service_name, group);
        entry->owner = this;
        entry->lease_count = 1;
        entry->retain(); // registry reference
        entries_.insert(*entry);
        ++entry_count_;
        ++live_entries_;

        auto subscription = naming_service_->subscribe(entry->service_name, entry->group, &on_notify, entry.get());
        if (!subscription) {
            NamingServiceError error = std::move(subscription.error());
            release_lease(*entry);
            return std::unexpected(std::move(error));
        }

        // subscribe() may synchronously replay a cached value, and StateOps
        // callbacks may re-enter this object while that replay is running.
        if (entry->phase == EntryPhase::Pending || entry->phase == EntryPhase::Ready) {
            entry->subscription = std::move(*subscription);
            return Lease(std::move(entry));
        }

        subscription->close();
        NamingServiceError error =
                entry->ready_error == ServiceReadyError::Shutdown ? stopping_error() : closed_before_acquire_error();
        release_lease(*entry);
        return std::unexpected(std::move(error));
    }

    [[nodiscard]] async::Task<void> shutdown() noexcept {
        FIBER_ASSERT(loop_->in_loop());
        if (!stopping_) {
            stopping_ = true;
            while (Entry *entry = entries_.minimum()) {
                EntryPtr hold(entry);
                retire_entry(*entry, ServiceReadyError::Shutdown);
            }
        }
        co_return;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entry_count_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    [[nodiscard]] static NamingServiceError stopping_error() {
        return NamingServiceError{
                .code = NamingServiceErrorCode::Shutdown,
                .io_error = common::IoErr::Canceled,
                .message = "service discovery is stopping",
        };
    }

    [[nodiscard]] static NamingServiceError closed_before_acquire_error() {
        return NamingServiceError{
                .code = NamingServiceErrorCode::Shutdown,
                .io_error = common::IoErr::Canceled,
                .message = "service discovery subscription closed during acquire",
        };
    }

    [[nodiscard]] static std::uint64_t key_hash(std::string_view service_name, std::string_view group) noexcept {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        constexpr std::uint64_t prime = 0x100000001b3ull;
        const auto mix = [&](std::string_view value) {
            for (unsigned char character: value) {
                hash ^= character;
                hash *= prime;
            }
            hash ^= 0x2f;
            hash *= prime;
        };
        mix(service_name);
        mix(group);
        return hash;
    }

    [[nodiscard]] static int compare_key(std::uint64_t left_hash, std::string_view left_service_name,
                                         std::string_view left_group, std::uint64_t right_hash,
                                         std::string_view right_service_name, std::string_view right_group) noexcept {
        if (left_hash != right_hash) {
            return left_hash < right_hash ? -1 : 1;
        }
        if (const int comparison = left_service_name.compare(right_service_name); comparison != 0) {
            return comparison < 0 ? -1 : 1;
        }
        if (const int comparison = left_group.compare(right_group); comparison != 0) {
            return comparison < 0 ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] EntryPtr find(std::string_view service_name, std::string_view group) noexcept {
        const std::uint64_t hash = key_hash(service_name, group);
        Entry *entry = entries_.root();
        while (entry != nullptr) {
            const int comparison =
                    compare_key(hash, service_name, group, entry->key_hash, entry->service_name, entry->group);
            if (comparison == 0) {
                return EntryPtr(entry);
            }
            entry = comparison < 0 ? entry_from_hook(entry->tree_hook.left) : entry_from_hook(entry->tree_hook.right);
        }
        return EntryPtr{};
    }

    [[nodiscard]] static EntryPtr make_entry(std::string_view service_name, std::string_view group) {
        static_assert(alignof(Entry) <= alignof(std::max_align_t));
        const std::size_t total = sizeof(Entry) + service_name.size() + group.size() + 2;
        void *memory = std::malloc(total);
        FIBER_ASSERT(memory != nullptr);
        auto *entry = new (memory) Entry();

        char *service_tail = reinterpret_cast<char *>(entry) + sizeof(Entry);
        if (!service_name.empty()) {
            std::memcpy(service_tail, service_name.data(), service_name.size());
        }
        service_tail[service_name.size()] = '\0';
        char *group_tail = service_tail + service_name.size() + 1;
        if (!group.empty()) {
            std::memcpy(group_tail, group.data(), group.size());
        }
        group_tail[group.size()] = '\0';

        entry->key_hash = key_hash(service_name, group);
        entry->service_name = std::string_view(service_tail, service_name.size());
        entry->group = std::string_view(group_tail, group.size());
        return EntryPtr(entry);
    }

    static void destroy_entry(Entry *entry) noexcept {
        FIBER_ASSERT(entry != nullptr);
        FIBER_ASSERT(!entry->tree_hook.linked());
        FIBER_ASSERT(entry->lease_count == 0);
        FIBER_ASSERT(entry->waiters_head == nullptr);
        FIBER_ASSERT(entry->waiters_tail == nullptr);
        FIBER_ASSERT(entry->subscription.closed());
        ServiceDiscovery *owner = entry->owner;
        FIBER_ASSERT(owner != nullptr);
        FIBER_ASSERT(owner->live_entries_ > 0);
        --owner->live_entries_;
        entry->~Entry();
        std::free(entry);
    }

    [[nodiscard]] static Entry *entry_from_hook(common::IntrusiveRbTreeHook *hook) noexcept {
        if (hook == nullptr || !hook->linked()) {
            return nullptr;
        }
        return reinterpret_cast<Entry *>(reinterpret_cast<std::uint8_t *>(hook) - offsetof(Entry, tree_hook));
    }

    static void on_notify(void *context, const SubscriptionResult<ServiceInfo> &result) noexcept {
        auto *entry = static_cast<Entry *>(context);
        FIBER_ASSERT(entry != nullptr);
        EntryPtr hold(entry);
        ServiceDiscovery *owner = entry->owner;
        FIBER_ASSERT(owner != nullptr);
        FIBER_ASSERT(owner->loop_->in_loop());

        if (entry->phase != EntryPhase::Pending && entry->phase != EntryPhase::Ready) {
            return;
        }
        if (result.kind == ResultKind::Closed) {
            owner->retire_entry(*entry, ServiceReadyError::Closed);
            return;
        }
        if (result.data != nullptr) {
            owner->apply(*entry, result.data);
        }
    }

    void apply(Entry &entry, const std::shared_ptr<const ServiceInfo> &snapshot) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        if (entry.phase != EntryPhase::Pending && entry.phase != EntryPhase::Ready) {
            return;
        }

        const ServiceKeyView key{.service_name = entry.service_name, .group = entry.group};
        if (entry.phase == EntryPhase::Pending) {
            entry.state = ops_.create(key, snapshot);
            FIBER_ASSERT(entry.state != nullptr);
            entry.phase = EntryPhase::Ready;
            ops_.on_change(*entry.state, key, ServiceChangeKind::Initial, true);
            resume_waiters(entry);
            return;
        }

        FIBER_ASSERT(entry.state != nullptr);
        const bool changed = ops_.update(*entry.state, key, snapshot);
        ops_.on_change(*entry.state, key, ServiceChangeKind::Update, changed);
    }

    void release_lease(Entry &entry) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        FIBER_ASSERT(entry.lease_count > 0);
        --entry.lease_count;
        if (entry.lease_count == 0) {
            retire_entry(entry, stopping_ ? ServiceReadyError::Shutdown : ServiceReadyError::Retired);
        }
    }

    void retire_entry(Entry &entry, ServiceReadyError reason) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        if (entry.phase == EntryPhase::Closed || entry.phase == EntryPhase::Retired) {
            return;
        }

        if (entry.tree_hook.linked()) {
            entries_.erase(entry);
            FIBER_ASSERT(entry_count_ > 0);
            --entry_count_;
            release_registry_reference(entry);
        }

        entry.ready_error = reason;
        entry.phase = reason == ServiceReadyError::Closed ? EntryPhase::Closed : EntryPhase::Retired;
        entry.subscription.close();
        if (entry.state != nullptr && !entry.state_retired) {
            entry.state_retired = true;
            const ServiceRetireReason retire_reason =
                    reason == ServiceReadyError::Closed     ? ServiceRetireReason::SubscriptionClosed
                    : reason == ServiceReadyError::Shutdown ? ServiceRetireReason::Shutdown
                                                            : ServiceRetireReason::Released;
            ops_.retire(*entry.state, ServiceKeyView{.service_name = entry.service_name, .group = entry.group},
                        retire_reason);
        }
        resume_waiters(entry);
    }

    static void release_registry_reference(Entry &entry) noexcept {
        FIBER_ASSERT(entry.ref_count > 0);
        if (--entry.ref_count == 0) {
            destroy_entry(&entry);
        }
    }

    void add_waiter(Entry &entry, ReadyWaiter &waiter) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        FIBER_ASSERT(entry.phase == EntryPhase::Pending);
        FIBER_ASSERT(!waiter.linked);
        waiter.previous = entry.waiters_tail;
        if (entry.waiters_tail != nullptr) {
            entry.waiters_tail->next = &waiter;
        } else {
            entry.waiters_head = &waiter;
        }
        entry.waiters_tail = &waiter;
        waiter.linked = true;
    }

    void remove_waiter(Entry &entry, ReadyWaiter &waiter) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        FIBER_ASSERT(waiter.linked);
        if (waiter.previous != nullptr) {
            waiter.previous->next = waiter.next;
        } else {
            entry.waiters_head = waiter.next;
        }
        if (waiter.next != nullptr) {
            waiter.next->previous = waiter.previous;
        } else {
            entry.waiters_tail = waiter.previous;
        }
        waiter.previous = nullptr;
        waiter.next = nullptr;
        waiter.linked = false;
    }

    void resume_waiters(Entry &entry) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        while (ReadyWaiter *waiter = entry.waiters_head) {
            remove_waiter(entry, *waiter);
            ReadyAwaiter *awaiter = waiter->awaiter;
            std::coroutine_handle<> coroutine = std::exchange(waiter->coroutine, {});
            FIBER_ASSERT(awaiter != nullptr);
            awaiter->completed_ = true;
            if (coroutine) {
                coroutine.resume();
            }
        }
    }

    event::EventLoop *loop_ = nullptr;
    NamingService *naming_service_ = nullptr;
    StateOps ops_;
    Registry entries_;
    std::size_t entry_count_ = 0;
    std::size_t live_entries_ = 0;
    bool stopping_ = false;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H
