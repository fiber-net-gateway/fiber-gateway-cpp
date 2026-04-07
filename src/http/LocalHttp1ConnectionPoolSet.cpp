#include "LocalHttp1ConnectionPoolSet.h"

#include <future>
#include <new>

#include "../common/Assert.h"

namespace fiber::http {

LocalHttp1ConnectionPoolSet::LocalHttp1ConnectionPoolSet(event::EventLoopGroup &group,
                                                         Options pool_options) noexcept
    : group_(&group),
      pool_options_(pool_options),
      storage_(std::make_unique<Slot[]>(group.size())) {
    for (std::size_t i = 0; i < group.size(); ++i) {
        auto *core = reinterpret_cast<Http1ConnectionPoolCore *>(storage_[i].storage);
        std::construct_at(core, group.at(i), pool_options_);
    }
}

LocalHttp1ConnectionPoolSet::LocalHttp1ConnectionPoolSet(event::EventLoopGroup &group) noexcept
    : LocalHttp1ConnectionPoolSet(group, Options{}) {}

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

void LocalHttp1ConnectionPoolSet::clear() noexcept {
    struct ClearOp {
        Http1ConnectionPoolCore *core = nullptr;
        std::promise<void> *done = nullptr;
        event::EventLoop::NotifyEntry notify{};

        static void run(ClearOp *op) {
            FIBER_ASSERT(op != nullptr);
            op->core->clear();
            op->done->set_value();
        }
    };

    if (!group_->running()) {
        for (std::size_t i = 0; i < group_->size(); ++i) {
            core_at(i).clear();
        }
        return;
    }

    auto *current = event::EventLoop::current_or_null();
    const bool in_group = current && current->group() == group_;
    std::unique_ptr<ClearOp[]> ops = std::make_unique<ClearOp[]>(group_->size());
    std::unique_ptr<std::promise<void>[]> promises = std::make_unique<std::promise<void>[]>(group_->size());
    std::unique_ptr<std::future<void>[]> futures = std::make_unique<std::future<void>[]>(group_->size());
    for (std::size_t i = 0; i < group_->size(); ++i) {
        futures[i] = promises[i].get_future();
        Http1ConnectionPoolCore &core = core_at(i);
        if (in_group && current == &core.loop()) {
            core.clear();
            promises[i].set_value();
            continue;
        }
        ops[i].core = &core;
        ops[i].done = &promises[i];
        core.loop().post<ClearOp, &ClearOp::notify, &ClearOp::run>(ops[i]);
    }
    for (std::size_t i = 0; i < group_->size(); ++i) {
        futures[i].wait();
    }
}

void LocalHttp1ConnectionPoolSet::shutdown() noexcept {
    struct ShutdownOp {
        Http1ConnectionPoolCore *core = nullptr;
        std::promise<void> *done = nullptr;
        event::EventLoop::NotifyEntry notify{};

        static void run(ShutdownOp *op) {
            FIBER_ASSERT(op != nullptr);
            op->core->shutdown();
            op->done->set_value();
        }
    };

    if (!group_->running()) {
        for (std::size_t i = 0; i < group_->size(); ++i) {
            core_at(i).shutdown();
        }
        return;
    }

    auto *current = event::EventLoop::current_or_null();
    const bool in_group = current && current->group() == group_;
    std::unique_ptr<ShutdownOp[]> ops = std::make_unique<ShutdownOp[]>(group_->size());
    std::unique_ptr<std::promise<void>[]> promises = std::make_unique<std::promise<void>[]>(group_->size());
    std::unique_ptr<std::future<void>[]> futures = std::make_unique<std::future<void>[]>(group_->size());
    for (std::size_t i = 0; i < group_->size(); ++i) {
        futures[i] = promises[i].get_future();
        Http1ConnectionPoolCore &core = core_at(i);
        if (in_group && current == &core.loop()) {
            core.shutdown();
            promises[i].set_value();
            continue;
        }
        ops[i].core = &core;
        ops[i].done = &promises[i];
        core.loop().post<ShutdownOp, &ShutdownOp::notify, &ShutdownOp::run>(ops[i]);
    }
    for (std::size_t i = 0; i < group_->size(); ++i) {
        futures[i].wait();
    }
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
