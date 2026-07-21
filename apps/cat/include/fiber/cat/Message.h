#ifndef FIBER_CAT_MESSAGE_H
#define FIBER_CAT_MESSAGE_H

#include <chrono>
#include <common/mem/BufPool.h>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string_view>
#include "Status.h"

namespace fiber::cat {

namespace detail {
struct DataEntry;
struct MessageNode;
struct TreeData;
} // namespace detail


class CatClientCore;

enum class MessageKind : std::uint8_t {
    Transaction,
    Event,
};

class MessageTrace;


struct MessageBase {
protected:
    struct DataChunk {
        DataChunk *next;
        std::size_t capacity;
        std::size_t offset;
        char content[1];
    };

public:
    MessageBase *next = nullptr;
    MessageKind kind = MessageKind::Event;
    MessageTrace *trace;
    std::string_view type;
    std::string_view name;
    std::string_view status{status::Incomplete};
    std::chrono::steady_clock::time_point commit_time;
    DataChunk *data_head_;
    DataChunk *data_tail_;
};
// Transaction or Event  using it in first field or extends it.

class MessageTrace {


private:
    // unordered_map<std::string_view, std::string_view> context; use hash table to implement it. alloc by BufPool
    mem::BufPool pool_; // used to alloc all the memory of message in this tree
    std::string_view current_id_;
    std::string_view root_id_;
    std::string_view parent_id_;
    MessageBase *root_ = nullptr;
    std::shared_ptr<CatClientCore> core_; // shared with Client. commit message to the io event loop by the core_
    std::size_t uncommitted_msg_size_ = 1;
};


class Event : public MessageBase {};

class Transaction : public MessageBase {

private:
    struct ChildrenChunk {
        ChildrenChunk *next = nullptr;
        std::size_t capacity;
        std::size_t offset;
        MessageBase children[1];
    };
    // duration_
    ChildrenChunk *children_head_ = nullptr;
    ChildrenChunk *children_tail_ = nullptr;
};

enum class RecordError : std::uint8_t {
    None,
    InvalidArgument,
    WrongEventLoop,
    Completed,
    LimitExceeded,
    NoMemory,
    WrongMetricKind,
};

struct RecordLimits {
    std::size_t max_messages = 2000;
    std::size_t max_children_per_transaction = 2048;
    std::size_t max_type_bytes = 256;
    std::size_t max_name_bytes = 4096;
    std::size_t max_status_bytes = 256;
    std::size_t max_data_bytes_per_message = 64 * 1024;
    std::size_t max_tree_bytes = 1024 * 1024;
};

struct DataEntryView {
    std::string_view key;
    std::string_view value;
    bool key_value = false;
};

class DataIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = DataEntryView;
    using difference_type = std::ptrdiff_t;

    DataIterator() noexcept = default;

    [[nodiscard]] DataEntryView operator*() const noexcept;
    DataIterator &operator++() noexcept;
    DataIterator operator++(int) noexcept;

    [[nodiscard]] bool operator==(const DataIterator &other) const noexcept { return entry_ == other.entry_; }

private:
    friend class DataRange;
    friend class MessageView;

    explicit DataIterator(const detail::DataEntry *entry) noexcept : entry_(entry) {}

    const detail::DataEntry *entry_ = nullptr;
};

class DataRange {
public:
    [[nodiscard]] DataIterator begin() const noexcept { return DataIterator(first_); }
    [[nodiscard]] DataIterator end() const noexcept { return {}; }
    [[nodiscard]] bool empty() const noexcept { return first_ == nullptr; }

private:
    friend class MessageView;

    explicit DataRange(const detail::DataEntry *first) noexcept : first_(first) {}

    const detail::DataEntry *first_ = nullptr;
};

class MessageView;

class ChildIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = MessageView;
    using difference_type = std::ptrdiff_t;

    ChildIterator() noexcept = default;

    [[nodiscard]] MessageView operator*() const noexcept;
    ChildIterator &operator++() noexcept;
    ChildIterator operator++(int) noexcept;

    [[nodiscard]] bool operator==(const ChildIterator &other) const noexcept { return node_ == other.node_; }

private:
    friend class ChildRange;
    friend class MessageView;

    explicit ChildIterator(const detail::MessageNode *node) noexcept : node_(node) {}

    const detail::MessageNode *node_ = nullptr;
};

class ChildRange {
public:
    [[nodiscard]] ChildIterator begin() const noexcept { return ChildIterator(first_); }
    [[nodiscard]] ChildIterator end() const noexcept { return {}; }
    [[nodiscard]] bool empty() const noexcept { return first_ == nullptr; }

private:
    friend class MessageView;

    explicit ChildRange(const detail::MessageNode *first) noexcept : first_(first) {}

    const detail::MessageNode *first_ = nullptr;
};

class MessageView {
public:
    MessageView() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return node_ != nullptr; }
    [[nodiscard]] MessageKind kind() const noexcept;
    [[nodiscard]] std::string_view type() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view status() const noexcept;
    [[nodiscard]] std::uint64_t timestamp_millis() const noexcept;
    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] bool success() const noexcept;
    [[nodiscard]] std::chrono::microseconds duration() const noexcept;
    [[nodiscard]] std::size_t child_count() const noexcept;
    [[nodiscard]] std::size_t data_size() const noexcept;
    [[nodiscard]] DataRange data() const noexcept;
    [[nodiscard]] ChildRange children() const noexcept;

private:
    friend class ChildIterator;
    friend class Event;
    friend class Transaction;

    explicit MessageView(const detail::MessageNode *node) noexcept : node_(node) {}

    const detail::MessageNode *node_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_MESSAGE_H
