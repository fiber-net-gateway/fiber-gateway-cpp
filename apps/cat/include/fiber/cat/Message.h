#ifndef FIBER_CAT_MESSAGE_H
#define FIBER_CAT_MESSAGE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace fiber::cat {

namespace detail {
struct DataEntry;
struct MessageNode;
struct TreeData;
} // namespace detail

enum class MessageKind : std::uint8_t {
    Transaction,
    Event,
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
