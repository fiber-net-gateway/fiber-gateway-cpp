#ifndef FIBER_HTTP_LOCAL_HTTP1_CONNECTION_POOL_SET_H
#define FIBER_HTTP_LOCAL_HTTP1_CONNECTION_POOL_SET_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>

#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/Assert.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoopGroup.h"
#include "Http1ConnectionPoolCore.h"

namespace fiber::http {

class LocalHttp1ConnectionPoolSet : public common::NonCopyable, public common::NonMovable {
public:
    using Lease = Http1ConnectionPoolCore::Lease;
    using Options = Http1ConnectionPoolCore::Options;

    explicit LocalHttp1ConnectionPoolSet(event::EventLoopGroup &group) noexcept;
    LocalHttp1ConnectionPoolSet(event::EventLoopGroup &group, Options pool_options) noexcept;
    ~LocalHttp1ConnectionPoolSet();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] async::Task<void> clear_async() noexcept;
    [[nodiscard]] async::Task<void> shutdown_async() noexcept;
    [[nodiscard]] Lease acquire(const HttpConnectionGroupKey &key) noexcept { return current_core().acquire(key); }

    [[nodiscard]] std::size_t size() const noexcept { return group_->size(); }
    [[nodiscard]] event::EventLoopGroup &group() noexcept { return *group_; }
    [[nodiscard]] const event::EventLoopGroup &group() const noexcept { return *group_; }
    [[nodiscard]] const Options &options() const noexcept { return pool_options_; }
    [[nodiscard]] event::EventLoop &loop() noexcept { return current_core().loop(); }
    [[nodiscard]] const event::EventLoop &loop() const noexcept { return current_core().loop(); }
    [[nodiscard]] std::size_t idle_total() const noexcept { return current_core().idle_total(); }
    [[nodiscard]] std::size_t group_count() const noexcept { return current_core().group_count(); }

private:
    class AdminAwaiter;

    struct alignas(Http1ConnectionPoolCore) Slot {
        std::byte storage[sizeof(Http1ConnectionPoolCore)];
    };

    [[nodiscard]] Http1ConnectionPoolCore &current_core() noexcept {
        auto *loop = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop != nullptr);
        return core_for(*loop);
    }
    [[nodiscard]] const Http1ConnectionPoolCore &current_core() const noexcept {
        auto *loop = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop != nullptr);
        return core_for(*loop);
    }
    [[nodiscard]] Http1ConnectionPoolCore &core_at(std::size_t index) noexcept {
        FIBER_ASSERT(index < group_->size());
        return *std::launder(reinterpret_cast<Http1ConnectionPoolCore *>(storage_[index].storage));
    }
    [[nodiscard]] const Http1ConnectionPoolCore &core_at(std::size_t index) const noexcept {
        FIBER_ASSERT(index < group_->size());
        return *std::launder(reinterpret_cast<const Http1ConnectionPoolCore *>(storage_[index].storage));
    }
    [[nodiscard]] Http1ConnectionPoolCore &core_for(const event::EventLoop &loop) noexcept;
    [[nodiscard]] const Http1ConnectionPoolCore &core_for(const event::EventLoop &loop) const noexcept;

    event::EventLoopGroup *group_ = nullptr;
    Options pool_options_{};
    std::unique_ptr<Slot[]> storage_{};
    std::atomic<bool> shutdown_requested_{false};
    std::mutex shutdown_mu_{};
    async::WaitGroup shutdown_wg_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_LOCAL_HTTP1_CONNECTION_POOL_SET_H
