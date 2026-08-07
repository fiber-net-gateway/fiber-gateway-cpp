#ifndef FIBER_NACOS_POOL_SNAPSHOT_H
#define FIBER_NACOS_POOL_SNAPSHOT_H

#include <cstddef>
#include <memory>

#include <fiber/common/mem/BufPool.h>

namespace fiber::nacos::detail {

template<typename T>
struct PoolSnapshotOwner {
    explicit PoolSnapshotOwner(std::size_t block_size = 4096) noexcept : pool(block_size) {}

    mem::BufPool pool;
    T data;
};

template<typename T>
[[nodiscard]] std::shared_ptr<PoolSnapshotOwner<T>> make_pool_snapshot_owner(std::size_t block_size = 4096) {
    return std::make_shared<PoolSnapshotOwner<T>>(block_size);
}

template<typename T>
[[nodiscard]] std::shared_ptr<const T> freeze_pool_snapshot(const std::shared_ptr<PoolSnapshotOwner<T>> &owner) {
    return std::shared_ptr<const T>(owner, &owner->data);
}

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_POOL_SNAPSHOT_H
