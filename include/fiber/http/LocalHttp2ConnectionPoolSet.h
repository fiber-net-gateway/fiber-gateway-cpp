#ifndef FIBER_HTTP_LOCAL_HTTP2_CONNECTION_POOL_SET_H
#define FIBER_HTTP_LOCAL_HTTP2_CONNECTION_POOL_SET_H

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
#include "Http2ConnectionPoolCore.h"

namespace fiber::http {

class LocalHttp2ConnectionPoolSet : public common::NonCopyable, public common::NonMovable {
public:
    using Lease = Http2ConnectionPoolCore::Lease;
    using Options = Http2ConnectionPoolCore::Options;

    explicit LocalHttp2ConnectionPoolSet(event::EventLoopGroup &group) noexcept;
    LocalHttp2ConnectionPoolSet(event::EventLoopGroup &group, Options pool_options) noexcept;
    ~LocalHttp2ConnectionPoolSet();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] async::Task<void> clear_async() noexcept;
    [[nodiscard]] async::Task<void> shutdown_async() noexcept;
    using Connector = Http2ConnectionPoolCore::Connector;
    [[nodiscard]] async::Task<common::IoResult<Lease>>
    acquire(HttpConnectionGroupKey key, Connector connector,
            std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        return current_core().acquire(std::move(key), connector, timeout);
    }
    [[nodiscard]] std::size_t connection_total() const noexcept { return current_core().connection_total(); }
    void set_conn_count_changed_callback(Http2ConnectionPoolCore::ConnCountChangedCallback cb, void *ctx) noexcept {
        current_core().set_conn_count_changed_callback(cb, ctx);
    }

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

    struct alignas(Http2ConnectionPoolCore) Slot {
        std::byte storage[sizeof(Http2ConnectionPoolCore)];
    };

    [[nodiscard]] Http2ConnectionPoolCore &current_core() noexcept {
        auto *loop = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop != nullptr);
        return core_for(*loop);
    }
    [[nodiscard]] const Http2ConnectionPoolCore &current_core() const noexcept {
        auto *loop = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop != nullptr);
        return core_for(*loop);
    }
    [[nodiscard]] Http2ConnectionPoolCore &core_at(std::size_t index) noexcept {
        FIBER_ASSERT(index < group_->size());
        return *std::launder(reinterpret_cast<Http2ConnectionPoolCore *>(storage_[index].storage));
    }
    [[nodiscard]] const Http2ConnectionPoolCore &core_at(std::size_t index) const noexcept {
        FIBER_ASSERT(index < group_->size());
        return *std::launder(reinterpret_cast<const Http2ConnectionPoolCore *>(storage_[index].storage));
    }
    [[nodiscard]] Http2ConnectionPoolCore &core_for(const event::EventLoop &loop) noexcept;
    [[nodiscard]] const Http2ConnectionPoolCore &core_for(const event::EventLoop &loop) const noexcept;

    event::EventLoopGroup *group_ = nullptr;
    Options pool_options_{};
    std::unique_ptr<Slot[]> storage_{};
    std::atomic<bool> shutdown_requested_{false};
    std::mutex shutdown_mu_{};
    async::WaitGroup shutdown_wg_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_LOCAL_HTTP2_CONNECTION_POOL_SET_H
