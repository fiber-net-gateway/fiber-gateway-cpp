#include "LocalHttp1ConnectionPoolSet.h"

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
    for (std::size_t i = 0; i < group_->size(); ++i) {
        core_at(i).clear();
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
