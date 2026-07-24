#include "LocalHttp1ConnectionPoolSet.h"

#include <atomic>
#include <coroutine>
#include <memory>
#include <new>

#include "../common/Assert.h"

namespace fiber::http {

class LocalHttp1ConnectionPoolSet::AdminAwaiter : public common::NonCopyable, public common::NonMovable {
public:
    enum class Operation : std::uint8_t { Clear, Shutdown };

    AdminAwaiter(LocalHttp1ConnectionPoolSet &set, Operation operation) noexcept : set_(&set), operation_(operation) {}

    bool await_ready() noexcept {
        if (!set_) {
            return true;
        }
        if (operation_ == Operation::Clear && set_->shutdown_requested_.load(std::memory_order_acquire)) {
            return true;
        }
        if (set_->group().running()) {
            return false;
        }
        for (std::size_t i = 0; i < set_->size(); ++i) {
            Http1ConnectionPoolCore &core = set_->core_at(i);
            if (operation_ == Operation::Clear) {
                core.clear();
            } else {
                core.shutdown();
            }
        }
        return true;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        FIBER_ASSERT(set_ != nullptr);
        handle_ = handle;
        caller_loop_ = event::EventLoop::current_or_null();
        FIBER_ASSERT(caller_loop_ != nullptr);

        const std::size_t shard_count = set_->size();
        if (shard_count == 0) {
            caller_loop_->post<AdminAwaiter, &AdminAwaiter::resume_notify_, &AdminAwaiter::resume_caller>(*this);
            return true;
        }

        remaining_.store(shard_count, std::memory_order_release);
        ops_ = std::make_unique<ShardOp[]>(shard_count);
        auto *current = event::EventLoop::current_or_null();

        // Run the caller's shard first. If it is the only shard, completing
        // inline avoids queuing a wakeup to the event loop that is currently
        // executing this await_suspend().
        if (current && current->group() == &set_->group() && current->has_group_index()) {
            Http1ConnectionPoolCore &core = set_->core_at(current->group_index());
            run_core(core);
            if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                handle_ = {};
                return false;
            }
        }

        for (std::size_t i = 0; i < shard_count; ++i) {
            Http1ConnectionPoolCore &core = set_->core_at(i);
            if (current == &core.loop()) {
                continue;
            }
            ops_[i].awaiter = this;
            ops_[i].core = &core;
            core.loop().post<ShardOp, &ShardOp::notify, &ShardOp::run>(ops_[i]);
        }
        return true;
    }

    void await_resume() noexcept {}

private:
    struct ShardOp {
        AdminAwaiter *awaiter = nullptr;
        Http1ConnectionPoolCore *core = nullptr;
        event::EventLoop::NotifyEntry notify{};

        static void run(ShardOp *op) noexcept {
            FIBER_ASSERT(op != nullptr);
            FIBER_ASSERT(op->awaiter != nullptr);
            FIBER_ASSERT(op->core != nullptr);
            op->awaiter->run_core(*op->core);
            op->awaiter->on_shard_done();
        }
    };

    void run_core(Http1ConnectionPoolCore &core) noexcept {
        if (operation_ == Operation::Clear) {
            core.clear();
        } else {
            core.shutdown();
        }
    }

    void on_shard_done() noexcept {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return;
        }
        FIBER_ASSERT(caller_loop_ != nullptr);
        caller_loop_->post<AdminAwaiter, &AdminAwaiter::resume_notify_, &AdminAwaiter::resume_caller>(*this);
    }

    static void resume_caller(AdminAwaiter *awaiter) noexcept {
        FIBER_ASSERT(awaiter != nullptr);
        if (!awaiter->handle_) {
            return;
        }
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        handle.resume();
    }

    LocalHttp1ConnectionPoolSet *set_ = nullptr;
    Operation operation_ = Operation::Clear;
    event::EventLoop *caller_loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    std::unique_ptr<ShardOp[]> ops_{};
    std::atomic<std::size_t> remaining_{0};
    event::EventLoop::NotifyEntry resume_notify_{};
};

LocalHttp1ConnectionPoolSet::LocalHttp1ConnectionPoolSet(event::EventLoopGroup &group, Options pool_options) noexcept :
    group_(&group), pool_options_(pool_options), storage_(std::make_unique<Slot[]>(group.size())) {
    for (std::size_t i = 0; i < group.size(); ++i) {
        auto *core = reinterpret_cast<Http1ConnectionPoolCore *>(storage_[i].storage);
        std::construct_at(core, group.at(i), pool_options_);
        core->set_external_shutdown_flag(&shutdown_requested_);
    }
}

LocalHttp1ConnectionPoolSet::LocalHttp1ConnectionPoolSet(event::EventLoopGroup &group) noexcept :
    LocalHttp1ConnectionPoolSet(group, Options{}) {}

LocalHttp1ConnectionPoolSet::~LocalHttp1ConnectionPoolSet() {
    for (std::size_t i = 0; i < group_->size(); ++i) {
        auto *core = reinterpret_cast<Http1ConnectionPoolCore *>(storage_[i].storage);
        std::destroy_at(core);
    }
}

bool LocalHttp1ConnectionPoolSet::init() noexcept {
    for (std::size_t i = 0; i < group_->size(); ++i) {
        if (!core_at(i).init()) {
            return false;
        }
    }
    return true;
}

async::Task<void> LocalHttp1ConnectionPoolSet::clear_async() noexcept {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
        co_return;
    }
    co_await AdminAwaiter(*this, AdminAwaiter::Operation::Clear);
}

async::Task<void> LocalHttp1ConnectionPoolSet::shutdown_async() noexcept {
    bool leader = false;
    {
        std::lock_guard guard(shutdown_mu_);
        if (!shutdown_requested_.load(std::memory_order_relaxed)) {
            shutdown_wg_.add();
            shutdown_requested_.store(true, std::memory_order_release);
            leader = true;
        }
    }

    if (!leader) {
        co_await shutdown_wg_.join();
        co_return;
    }

    co_await AdminAwaiter(*this, AdminAwaiter::Operation::Shutdown);
    shutdown_wg_.done();
}

Http1ConnectionPoolCore &LocalHttp1ConnectionPoolSet::core_for(const event::EventLoop &loop) noexcept {
    FIBER_ASSERT(loop.group() == group_);
    FIBER_ASSERT(loop.has_group_index());
    const std::size_t index = loop.group_index();
    FIBER_ASSERT(index < group_->size());
    FIBER_ASSERT(&group_->at(index) == &loop);
    return core_at(index);
}

const Http1ConnectionPoolCore &LocalHttp1ConnectionPoolSet::core_for(const event::EventLoop &loop) const noexcept {
    FIBER_ASSERT(loop.group() == group_);
    FIBER_ASSERT(loop.has_group_index());
    const std::size_t index = loop.group_index();
    FIBER_ASSERT(index < group_->size());
    FIBER_ASSERT(&group_->at(index) == &loop);
    return core_at(index);
}

} // namespace fiber::http
