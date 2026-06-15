#ifndef FIBER_COMMON_MEM_IOBUFCHAIN_H
#define FIBER_COMMON_MEM_IOBUFCHAIN_H

#include <cstddef>
#include <cstdint>
#include <sys/uio.h>

#include "IoBuf.h"

namespace fiber::mem {

inline constexpr std::size_t kIoBufNodePoolMaxCached = 1000;

struct IoBufNode {
    std::uint64_t offset = 0;
    std::uint8_t state = 0;
    IoBuf buf{};
    IoBufNode *next = nullptr;
};

class IoBufNodePool {
public:
    IoBufNodePool() noexcept = default;
    ~IoBufNodePool();

    IoBufNodePool(const IoBufNodePool &) = delete;
    IoBufNodePool &operator=(const IoBufNodePool &) = delete;
    IoBufNodePool(IoBufNodePool &&) = delete;
    IoBufNodePool &operator=(IoBufNodePool &&) = delete;

    [[nodiscard]] IoBufNode *alloc() noexcept;
    void release(IoBufNode *node) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t cached_count() const noexcept { return cached_count_; }

private:
    IoBufNode *free_head_ = nullptr;
    std::size_t cached_count_ = 0;
};

class IoBufChain {
public:
    IoBufChain() noexcept = default;
    explicit IoBufChain(IoBufNodePool &node_pool) noexcept;
    ~IoBufChain();

    IoBufChain(const IoBufChain &) = delete;
    IoBufChain &operator=(const IoBufChain &) = delete;

    IoBufChain(IoBufChain &&other) noexcept;
    IoBufChain &operator=(IoBufChain &&other) noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t readable_bytes() const noexcept;
    [[nodiscard]] std::size_t writable_bytes() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] IoBufNodePool &node_pool() noexcept;
    [[nodiscard]] const IoBufNodePool &node_pool() const noexcept;
    [[nodiscard]] bool bound() const noexcept;
    [[nodiscard]] bool same_pool(const IoBufChain &other) const noexcept;
    void bind_node_pool(IoBufNodePool &node_pool) noexcept;

    bool append(IoBuf &&buf) noexcept;
    bool prepend(IoBuf &&buf) noexcept;
    bool append_node(IoBufNode *node) noexcept;
    bool prepend_node(IoBufNode *node) noexcept;
    [[nodiscard]] bool retain_prefix(std::size_t bytes, IoBufChain &out) const noexcept;
    bool take_prefix(std::size_t bytes, IoBufChain &dst) noexcept;
    void clear() noexcept;
    void consume(std::size_t bytes) noexcept;
    void drop_empty_front() noexcept;
    void consume_and_compact(std::size_t bytes) noexcept;
    void commit(std::size_t bytes) noexcept;
    void mark_complete() noexcept;
    void clear_complete() noexcept;
    [[nodiscard]] IoBufNode *pop_front_node() noexcept;

    [[nodiscard]] int fill_write_iov(struct iovec *iov, int max_iov) const noexcept;
    [[nodiscard]] int fill_read_iov(struct iovec *iov, int max_iov) const noexcept;

    [[nodiscard]] IoBuf *front() noexcept;
    [[nodiscard]] const IoBuf *front() const noexcept;
    [[nodiscard]] IoBuf *first_readable() noexcept;
    [[nodiscard]] const IoBuf *first_readable() const noexcept;
    [[nodiscard]] IoBuf *first_writable() noexcept;
    [[nodiscard]] const IoBuf *first_writable() const noexcept;

private:
    void release_nodes(IoBufNode *node) noexcept;
    static void reset_node_for_chain(IoBufNode &node) noexcept;

    IoBufNode *head_ = nullptr;
    IoBufNode *tail_ = nullptr;
    std::size_t size_ = 0;
    std::size_t readable_bytes_ = 0;
    std::size_t writable_bytes_ = 0;
    IoBufNodePool *node_pool_ = nullptr;
    bool complete_ = false;
};

} // namespace fiber::mem

#endif // FIBER_COMMON_MEM_IOBUFCHAIN_H
