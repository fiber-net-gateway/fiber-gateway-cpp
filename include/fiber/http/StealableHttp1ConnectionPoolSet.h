#ifndef FIBER_HTTP_STEALABLE_HTTP1_CONNECTION_POOL_SET_H
#define FIBER_HTTP_STEALABLE_HTTP1_CONNECTION_POOL_SET_H

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>

#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoopGroup.h"
#include "Http1ConnectionGroupHintTable.h"
#include "Http1ConnectionPoolCore.h"

namespace fiber::http {

class StealableHttp1ConnectionPoolSet : public common::NonCopyable, public common::NonMovable {
public:
    using Options = Http1ConnectionPoolCore::Options;

    class AcquireAwaiter;

    class Lease : public common::NonCopyable {
    public:
        Lease() noexcept = default;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        ~Lease();

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] bool has_connection() const noexcept;
        [[nodiscard]] bool hit() const noexcept;

        [[nodiscard]] Http1ClientConnection *get() noexcept;
        [[nodiscard]] const Http1ClientConnection *get() const noexcept;

        [[nodiscard]] Http1ClientConnection &connection() noexcept;
        [[nodiscard]] const Http1ConnectionGroupKey &key() const noexcept;
        [[nodiscard]] common::IoResult<Http1ClientConnection *> emplace_connection() noexcept;
        void reset() noexcept;

    private:
        friend class StealableHttp1ConnectionPoolSet;

        enum class Kind : std::uint8_t { Empty, Local, Remote };

        explicit Lease(Http1ConnectionPoolCore::Lease &&local) noexcept;
        Lease(Http1ConnectionPoolCore &home_core, Http1ConnectionPoolEntry &entry,
              const Http1ConnectionGroupKey &key) noexcept;

        Kind kind_ = Kind::Empty;
        Http1ConnectionPoolCore::Lease local_{};
        Http1ConnectionPoolEntry *entry_ = nullptr;
        Http1ConnectionPoolCore *home_core_ = nullptr;
        std::optional<Http1ConnectionGroupKey> key_{};
    };

    explicit StealableHttp1ConnectionPoolSet(event::EventLoopGroup &group) noexcept;
    StealableHttp1ConnectionPoolSet(event::EventLoopGroup &group, Options pool_options) noexcept;
    ~StealableHttp1ConnectionPoolSet();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] async::Task<void> clear_async() noexcept;
    [[nodiscard]] async::Task<void> shutdown_async() noexcept;
    [[nodiscard]] AcquireAwaiter acquire(const Http1ConnectionGroupKey &key) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return group_->size(); }
    [[nodiscard]] event::EventLoopGroup &group() noexcept { return *group_; }
    [[nodiscard]] const event::EventLoopGroup &group() const noexcept { return *group_; }
    [[nodiscard]] const Options &options() const noexcept { return pool_options_; }

private:
    class AdminAwaiter;

    struct Shard {
        Shard(event::EventLoop &loop, Options pool_options) noexcept : core(loop, pool_options), hint() {
            core.set_idle_count_changed_callback(&Shard::on_idle_count_changed, this);
        }

        ~Shard() { core.clear_idle_count_changed_callback(); }

        static void on_idle_count_changed(void *ctx, const Http1ConnectionGroupKey &key,
                                          std::size_t idle_count) noexcept {
            auto *self = static_cast<Shard *>(ctx);
            FIBER_ASSERT(self != nullptr);
            const std::size_t target =
                    idle_count < static_cast<std::size_t>(Http1ConnectionGroupHintTable::kMaxApproxCount)
                            ? idle_count
                            : static_cast<std::size_t>(Http1ConnectionGroupHintTable::kMaxApproxCount);
            std::size_t current = self->hint.probe(key).approx_count;
            while (current < target) {
                self->hint.note_idle_add(key);
                ++current;
            }
            while (current > target) {
                self->hint.note_idle_remove(key);
                --current;
            }
        }

        Http1ConnectionPoolCore core;
        Http1ConnectionGroupHintTable hint;
    };

    struct alignas(Shard) ShardSlot {
        std::byte storage[sizeof(Shard)];
        ShardSlot *next = nullptr;
    };

    [[nodiscard]] Shard &current_shard() noexcept;
    [[nodiscard]] const Shard &current_shard() const noexcept;
    [[nodiscard]] ShardSlot &current_slot() noexcept;
    [[nodiscard]] const ShardSlot &current_slot() const noexcept;
    [[nodiscard]] Shard &shard_at(std::size_t index) noexcept;
    [[nodiscard]] const Shard &shard_at(std::size_t index) const noexcept;
    [[nodiscard]] ShardSlot &slot_at(std::size_t index) noexcept;
    [[nodiscard]] const ShardSlot &slot_at(std::size_t index) const noexcept;
    [[nodiscard]] bool begin_remote_acquire() noexcept;
    void finish_remote_acquire() noexcept;

    event::EventLoopGroup *group_ = nullptr;
    Options pool_options_{};
    std::unique_ptr<ShardSlot[]> storage_{};
    std::atomic<bool> shutdown_requested_{false};
#if FIBER_ENABLE_BENCHMARK_TRACE
    void trace_remote_hit() noexcept;
    void trace_report() const noexcept;

    std::atomic<std::uint64_t> trace_local_hit_{0};
    std::atomic<std::uint64_t> trace_remote_attempt_{0};
    std::atomic<std::uint64_t> trace_remote_hit_{0};
    std::atomic<std::uint64_t> trace_remote_attempt_miss_{0};
    std::atomic<std::uint64_t> trace_no_candidate_{0};
#endif
    std::mutex shutdown_mu_{};
    async::WaitGroup shutdown_wg_{};
    async::WaitGroup active_acquire_wg_{};
};

class StealableHttp1ConnectionPoolSet::AcquireAwaiter : public common::NonCopyable {
public:
    AcquireAwaiter(StealableHttp1ConnectionPoolSet &set, const Http1ConnectionGroupKey &key) noexcept;
    AcquireAwaiter(AcquireAwaiter &&) = delete;
    AcquireAwaiter &operator=(AcquireAwaiter &&) = delete;
    ~AcquireAwaiter() noexcept;

    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    Lease await_resume() noexcept;
    [[nodiscard]] bool completed() const noexcept { return completed_; }

private:
    friend class StealableHttp1ConnectionPoolSet;

    class State;

    [[nodiscard]] Shard &target_shard() const noexcept;
    [[nodiscard]] bool prepare() noexcept;
    [[nodiscard]] bool advance_to_candidate() noexcept;

    StealableHttp1ConnectionPoolSet *set_ = nullptr;
    std::optional<Http1ConnectionGroupKey> key_{};
    Http1ConnectionPoolCore::Lease local_fallback_{};
    Lease result_{};
    ShardSlot *home_slot_ = nullptr;
    ShardSlot *cursor_ = nullptr;
    event::EventLoop *caller_loop_ = nullptr;
    State *state_ = nullptr;
    bool prepared_ = false;
    bool completed_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_STEALABLE_HTTP1_CONNECTION_POOL_SET_H
