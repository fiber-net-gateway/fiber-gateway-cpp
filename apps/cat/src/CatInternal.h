#ifndef FIBER_CAT_INTERNAL_H
#define FIBER_CAT_INTERNAL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include <common/mem/BufPool.h>
#include <event/EventLoop.h>
#include <fiber/cat/Message.h>
#include <fiber/cat/Status.h>

namespace fiber::cat::detail {

struct StringRef {
    const char *data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {data, size}; }
};

struct DataEntry {
    StringRef key;
    StringRef value;
    DataEntry *next = nullptr;
    bool key_value = false;
};

struct MessageNode {
    MessageKind kind = MessageKind::Event;
    StringRef type;
    StringRef name;
    StringRef status{status::Success.data(), status::Success.size()};
    DataEntry *data_head = nullptr;
    DataEntry *data_tail = nullptr;
    MessageNode *next_sibling = nullptr;
    std::uint64_t timestamp_millis = 0;
    std::size_t data_size = 0;
    bool completed = false;
};

struct TransactionNode {
    MessageNode message{.kind = MessageKind::Transaction};
    MessageNode *children_head = nullptr;
    MessageNode *children_tail = nullptr;
    std::chrono::steady_clock::time_point duration_start{};
    std::chrono::microseconds duration{};
    std::size_t child_count = 0;
    bool explicit_duration = false;
};

struct TreeData {
    TreeData(event::EventLoop &owner, RecordLimits limits) noexcept;

    event::EventLoop *owner = nullptr;
    RecordLimits limits;
    mem::BufPool pool;
    MessageNode *root = nullptr;
    std::chrono::steady_clock::time_point steady_base{};
    std::uint64_t wall_base_millis = 0;
    std::size_t payload_bytes = 0;
    std::size_t message_count = 0;
    std::size_t open_messages = 0;
    std::size_t refs = 1;
    bool has_problem = false;
};

[[nodiscard]] bool valid_limits(const RecordLimits &limits) noexcept;
[[nodiscard]] bool on_owner_loop(const TreeData &tree) noexcept;
[[nodiscard]] std::uint64_t current_timestamp_millis(const TreeData &tree) noexcept;

[[nodiscard]] std::expected<TreeData *, RecordError>
create_transaction_tree(std::string_view type, std::string_view name, RecordLimits limits) noexcept;
[[nodiscard]] std::expected<TreeData *, RecordError> create_event_tree(std::string_view type, std::string_view name,
                                                                       RecordLimits limits) noexcept;

[[nodiscard]] std::expected<TransactionNode *, RecordError>
create_transaction(TreeData &tree, TransactionNode *parent, std::string_view type, std::string_view name) noexcept;
[[nodiscard]] std::expected<MessageNode *, RecordError>
create_event(TreeData &tree, TransactionNode *parent, std::string_view type, std::string_view name) noexcept;

RecordError add_data(TreeData &tree, MessageNode &message, std::string_view data) noexcept;
RecordError add_data(TreeData &tree, MessageNode &message, std::string_view key, std::string_view value) noexcept;
RecordError set_status(TreeData &tree, MessageNode &message, std::string_view value) noexcept;
RecordError set_timestamp(TreeData &tree, MessageNode &message, std::uint64_t timestamp_millis) noexcept;
RecordError set_duration(TreeData &tree, TransactionNode &transaction, std::chrono::microseconds duration) noexcept;
RecordError complete(TreeData &tree, MessageNode &message) noexcept;
void complete_incomplete(TreeData &tree, MessageNode &message) noexcept;

void retain(TreeData &tree) noexcept;
void release(TreeData *tree) noexcept;
[[nodiscard]] bool ready(const TreeData &tree) noexcept;

[[nodiscard]] TransactionNode *as_transaction(MessageNode *message) noexcept;
[[nodiscard]] const TransactionNode *as_transaction(const MessageNode *message) noexcept;

} // namespace fiber::cat::detail

#endif // FIBER_CAT_INTERNAL_H
