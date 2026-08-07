#ifndef FIBER_COMMON_MEM_IOBUF_H
#define FIBER_COMMON_MEM_IOBUF_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace fiber::mem {

class IoBuf;

class IoBufStorageBudget {
public:
    IoBufStorageBudget() noexcept = default;
    explicit IoBufStorageBudget(std::size_t limit, IoBufStorageBudget *parent = nullptr) noexcept;
    ~IoBufStorageBudget();

    IoBufStorageBudget(const IoBufStorageBudget &) = delete;
    IoBufStorageBudget &operator=(const IoBufStorageBudget &) = delete;
    IoBufStorageBudget(IoBufStorageBudget &&) = delete;
    IoBufStorageBudget &operator=(IoBufStorageBudget &&) = delete;

    void init(std::size_t limit, IoBufStorageBudget *parent = nullptr) noexcept;

    [[nodiscard]] bool try_retain(const IoBuf &buf, std::uint32_t refs = 1) noexcept;
    void release(const IoBuf &buf, std::uint32_t refs = 1) noexcept;

    [[nodiscard]] bool compatible(const IoBuf &buf) const noexcept;
    [[nodiscard]] bool retains(const IoBuf &buf) const noexcept;
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
    [[nodiscard]] std::size_t retained_capacity() const noexcept { return retained_capacity_; }
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] std::size_t rejected_count() const noexcept { return rejected_count_; }

private:
    [[nodiscard]] bool try_reserve(std::size_t bytes) noexcept;
    void unreserve(std::size_t bytes) noexcept;

    IoBufStorageBudget *parent_ = nullptr;
    std::size_t limit_ = std::numeric_limits<std::size_t>::max();
    std::size_t retained_capacity_ = 0;
    std::size_t high_water_ = 0;
    std::size_t rejected_count_ = 0;
};

class IoBuf {
public:
    IoBuf() noexcept = default;
    ~IoBuf();

    IoBuf(const IoBuf &other) noexcept;
    IoBuf &operator=(const IoBuf &other) noexcept;

    IoBuf(IoBuf &&other) noexcept;
    IoBuf &operator=(IoBuf &&other) noexcept;

    [[nodiscard]] static IoBuf allocate(std::size_t capacity) noexcept;
    [[nodiscard]] static IoBuf allocate_trackable(std::size_t capacity) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t view_size() const noexcept;
    [[nodiscard]] std::size_t readable() const noexcept;
    [[nodiscard]] std::size_t writable() const noexcept;
    [[nodiscard]] std::size_t headroom() const noexcept;
    [[nodiscard]] std::size_t tailroom() const noexcept;
    [[nodiscard]] bool unique() const noexcept;
    [[nodiscard]] std::uint32_t use_count() const noexcept;
    [[nodiscard]] bool storage_trackable() const noexcept;

    [[nodiscard]] std::uint8_t *data() noexcept;
    [[nodiscard]] const std::uint8_t *data() const noexcept;
    [[nodiscard]] std::uint8_t *view_begin() noexcept;
    [[nodiscard]] const std::uint8_t *view_begin() const noexcept;
    [[nodiscard]] std::uint8_t *view_end() noexcept;
    [[nodiscard]] const std::uint8_t *view_end() const noexcept;
    [[nodiscard]] std::uint8_t *readable_data() noexcept;
    [[nodiscard]] const std::uint8_t *readable_data() const noexcept;
    [[nodiscard]] std::uint8_t *writable_data() noexcept;
    [[nodiscard]] const std::uint8_t *writable_data() const noexcept;

    void clear() noexcept;
    void reset() noexcept;
    void consume(std::size_t bytes) noexcept;
    void commit(std::size_t bytes) noexcept;
    void swap(IoBuf &other) noexcept;

    [[nodiscard]] IoBuf retain_slice(std::size_t offset, std::size_t len) const noexcept;
    [[nodiscard]] IoBuf unsafe_retain_slice(std::size_t offset, std::size_t len) const noexcept;
    [[nodiscard]] IoBuf retain_storage_slice(std::size_t offset, std::size_t len) const noexcept;
    [[nodiscard]] bool same_storage(const IoBuf &other) const noexcept;
    bool try_merge_adjacent(IoBuf &&next) noexcept;

private:
    struct ControlBlock;
    struct StorageRetention;

    explicit IoBuf(ControlBlock *control, std::uint8_t *view_begin, std::uint8_t *view_end, std::uint8_t *pos,
                   std::uint8_t *last) noexcept;

    [[nodiscard]] IoBuf retain_slice_impl(std::size_t offset, std::size_t len, bool safe) const noexcept;
    [[nodiscard]] static IoBuf allocate_impl(std::size_t capacity, bool trackable) noexcept;

    static std::uint8_t *storage_begin(ControlBlock *control) noexcept;
    static const std::uint8_t *storage_begin(const ControlBlock *control) noexcept;
    static StorageRetention *storage_retention(ControlBlock *control) noexcept;
    static const StorageRetention *storage_retention(const ControlBlock *control) noexcept;
    static void retain(ControlBlock *control) noexcept;
    static void unsafe_retain(ControlBlock *control) noexcept;
    static void release(ControlBlock *control) noexcept;

    void reset_handle() noexcept;

    ControlBlock *control_ = nullptr;
    std::uint8_t *view_begin_ = nullptr;
    std::uint8_t *view_end_ = nullptr;
    std::uint8_t *pos_ = nullptr;
    std::uint8_t *last_ = nullptr;

    friend class IoBufStorageBudget;
};

} // namespace fiber::mem

#endif // FIBER_COMMON_MEM_IOBUF_H
