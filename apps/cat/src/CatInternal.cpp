#include "CatInternal.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <limits>
#include <new>

#include <common/Assert.h>

namespace fiber::cat::detail {

namespace {

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool can_charge(const TreeData &tree, std::size_t bytes) noexcept {
    return tree.payload_bytes <= tree.limits.max_tree_bytes && bytes <= tree.limits.max_tree_bytes - tree.payload_bytes;
}

StringRef copy_string(TreeData &tree, std::string_view value) noexcept {
    if (value.empty()) {
        return {"", 0};
    }
    auto *copy = static_cast<char *>(tree.pool.alloc(value.size(), alignof(char)));
    if (!copy) {
        return {};
    }
    std::copy(value.begin(), value.end(), copy);
    return {copy, value.size()};
}

RecordError validate_message(const TreeData &tree, std::string_view type, std::string_view name,
                             std::size_t node_size) noexcept {
    if (type.size() > tree.limits.max_type_bytes || name.size() > tree.limits.max_name_bytes) {
        return RecordError::LimitExceeded;
    }
    if (tree.message_count >= tree.limits.max_messages) {
        return RecordError::LimitExceeded;
    }
    std::size_t charge = 0;
    if (!checked_add(node_size, type.size(), charge) || !checked_add(charge, name.size(), charge) ||
        !can_charge(tree, charge)) {
        return RecordError::LimitExceeded;
    }
    return RecordError::None;
}

void attach_child(TransactionNode &parent, MessageNode &child) noexcept {
    if (parent.children_tail) {
        parent.children_tail->next_sibling = &child;
    } else {
        parent.children_head = &child;
    }
    parent.children_tail = &child;
    ++parent.child_count;
}

template<typename Node>
Node *allocate_message(TreeData &tree, std::string_view type, std::string_view name) noexcept {
    auto *storage = tree.pool.alloc<Node>();
    if (!storage) {
        return nullptr;
    }
    auto *node = new (storage) Node{};
    StringRef type_copy = copy_string(tree, type);
    if (!type.empty() && !type_copy.data) {
        return nullptr;
    }
    StringRef name_copy = copy_string(tree, name);
    if (!name.empty() && !name_copy.data) {
        return nullptr;
    }
    if constexpr (std::same_as<Node, TransactionNode>) {
        node->message.type = type_copy;
        node->message.name = name_copy;
        node->message.timestamp_millis = current_timestamp_millis(tree);
        node->duration_start = event::EventLoop::current().now();
    } else {
        node->type = type_copy;
        node->name = name_copy;
        node->timestamp_millis = current_timestamp_millis(tree);
    }
    tree.payload_bytes += sizeof(Node) + type.size() + name.size();
    ++tree.message_count;
    ++tree.open_messages;
    return node;
}

std::expected<TreeData *, RecordError> create_tree(RecordLimits limits) noexcept {
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (!loop) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (!valid_limits(limits)) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    auto *tree = new (std::nothrow) TreeData(*loop, limits);
    if (!tree) {
        return std::unexpected(RecordError::NoMemory);
    }
    return tree;
}

RecordError prepare_data_entry(TreeData &tree, MessageNode &message, std::string_view key, std::string_view value,
                               bool key_value, std::size_t rendered_size) noexcept {
    if (!on_owner_loop(tree)) {
        return RecordError::WrongEventLoop;
    }
    if (message.completed) {
        return RecordError::Completed;
    }
    const std::size_t separator_size = message.data_head ? 1 : 0;
    if (message.data_size > tree.limits.max_data_bytes_per_message ||
        rendered_size > tree.limits.max_data_bytes_per_message - message.data_size ||
        separator_size > tree.limits.max_data_bytes_per_message - message.data_size - rendered_size) {
        return RecordError::LimitExceeded;
    }
    std::size_t charge = 0;
    if (!checked_add(sizeof(DataEntry), key.size(), charge) || !checked_add(charge, value.size(), charge) ||
        !can_charge(tree, charge)) {
        return RecordError::LimitExceeded;
    }
    auto *storage = tree.pool.alloc<DataEntry>();
    if (!storage) {
        return RecordError::NoMemory;
    }
    auto *entry = new (storage) DataEntry{};
    StringRef key_copy = copy_string(tree, key);
    if (!key.empty() && !key_copy.data) {
        return RecordError::NoMemory;
    }
    StringRef value_copy = copy_string(tree, value);
    if (!value.empty() && !value_copy.data) {
        return RecordError::NoMemory;
    }
    entry->key = key_copy;
    entry->value = value_copy;
    entry->key_value = key_value;
    if (message.data_tail) {
        message.data_tail->next = entry;
    } else {
        message.data_head = entry;
    }
    message.data_tail = entry;
    message.data_size += separator_size + rendered_size;
    tree.payload_bytes += charge;
    return RecordError::None;
}

} // namespace

TreeData::TreeData(event::EventLoop &owner_value, RecordLimits limits_value) noexcept :
    owner(&owner_value), limits(limits_value), steady_base(owner_value.now()) {
    const auto wall_now = std::chrono::system_clock::now().time_since_epoch();
    const auto wall_millis = std::chrono::duration_cast<std::chrono::milliseconds>(wall_now).count();
    wall_base_millis = wall_millis > 0 ? static_cast<std::uint64_t>(wall_millis) : 0;
}

bool valid_limits(const RecordLimits &limits) noexcept {
    return limits.max_messages > 0 && limits.max_children_per_transaction > 0 && limits.max_type_bytes > 0 &&
           limits.max_name_bytes > 0 && limits.max_status_bytes > 0 && limits.max_data_bytes_per_message > 0 &&
           limits.max_tree_bytes >= sizeof(TransactionNode);
}

bool on_owner_loop(const TreeData &tree) noexcept { return tree.owner && tree.owner->in_loop(); }

std::uint64_t current_timestamp_millis(const TreeData &tree) noexcept {
    FIBER_ASSERT(on_owner_loop(tree));
    const auto delta =
            std::chrono::duration_cast<std::chrono::milliseconds>(event::EventLoop::current().now() - tree.steady_base)
                    .count();
    if (delta <= 0) {
        return tree.wall_base_millis;
    }
    const auto increment = static_cast<std::uint64_t>(delta);
    if (increment > std::numeric_limits<std::uint64_t>::max() - tree.wall_base_millis) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return tree.wall_base_millis + increment;
}

std::expected<TreeData *, RecordError> create_transaction_tree(std::string_view type, std::string_view name,
                                                               RecordLimits limits) noexcept {
    auto created = create_tree(limits);
    if (!created) {
        return created;
    }
    TreeData *tree = *created;
    auto root = create_transaction(*tree, nullptr, type, name);
    if (!root) {
        delete tree;
        return std::unexpected(root.error());
    }
    tree->root = &(*root)->message;
    return tree;
}

std::expected<TreeData *, RecordError> create_event_tree(std::string_view type, std::string_view name,
                                                         RecordLimits limits) noexcept {
    auto created = create_tree(limits);
    if (!created) {
        return created;
    }
    TreeData *tree = *created;
    auto root = create_event(*tree, nullptr, type, name);
    if (!root) {
        delete tree;
        return std::unexpected(root.error());
    }
    tree->root = *root;
    return tree;
}

std::expected<TransactionNode *, RecordError>
create_transaction(TreeData &tree, TransactionNode *parent, std::string_view type, std::string_view name) noexcept {
    if (!on_owner_loop(tree)) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (parent && parent->message.completed) {
        return std::unexpected(RecordError::Completed);
    }
    if (parent && parent->child_count >= tree.limits.max_children_per_transaction) {
        return std::unexpected(RecordError::LimitExceeded);
    }
    const RecordError validation = validate_message(tree, type, name, sizeof(TransactionNode));
    if (validation != RecordError::None) {
        return std::unexpected(validation);
    }
    TransactionNode *node = allocate_message<TransactionNode>(tree, type, name);
    if (!node) {
        return std::unexpected(RecordError::NoMemory);
    }
    if (parent) {
        attach_child(*parent, node->message);
    }
    return node;
}

std::expected<MessageNode *, RecordError> create_event(TreeData &tree, TransactionNode *parent, std::string_view type,
                                                       std::string_view name) noexcept {
    if (!on_owner_loop(tree)) {
        return std::unexpected(RecordError::WrongEventLoop);
    }
    if (parent && parent->message.completed) {
        return std::unexpected(RecordError::Completed);
    }
    if (parent && parent->child_count >= tree.limits.max_children_per_transaction) {
        return std::unexpected(RecordError::LimitExceeded);
    }
    const RecordError validation = validate_message(tree, type, name, sizeof(MessageNode));
    if (validation != RecordError::None) {
        return std::unexpected(validation);
    }
    MessageNode *node = allocate_message<MessageNode>(tree, type, name);
    if (!node) {
        return std::unexpected(RecordError::NoMemory);
    }
    if (parent) {
        attach_child(*parent, *node);
    }
    return node;
}

RecordError add_data(TreeData &tree, MessageNode &message, std::string_view data) noexcept {
    return prepare_data_entry(tree, message, data, {}, false, data.size());
}

RecordError add_data(TreeData &tree, MessageNode &message, std::string_view key, std::string_view value) noexcept {
    std::size_t rendered_size = 0;
    if (!checked_add(key.size(), 1, rendered_size) || !checked_add(rendered_size, value.size(), rendered_size)) {
        return RecordError::LimitExceeded;
    }
    return prepare_data_entry(tree, message, key, value, true, rendered_size);
}

RecordError set_status(TreeData &tree, MessageNode &message, std::string_view value) noexcept {
    if (!on_owner_loop(tree)) {
        return RecordError::WrongEventLoop;
    }
    if (message.completed) {
        return RecordError::Completed;
    }
    if (value.size() > tree.limits.max_status_bytes || !can_charge(tree, value.size())) {
        return RecordError::LimitExceeded;
    }
    StringRef copy = copy_string(tree, value);
    if (!value.empty() && !copy.data) {
        return RecordError::NoMemory;
    }
    message.status = copy;
    tree.payload_bytes += value.size();
    return RecordError::None;
}

RecordError set_timestamp(TreeData &tree, MessageNode &message, std::uint64_t timestamp_millis) noexcept {
    if (!on_owner_loop(tree)) {
        return RecordError::WrongEventLoop;
    }
    if (message.completed) {
        return RecordError::Completed;
    }
    message.timestamp_millis = timestamp_millis;
    return RecordError::None;
}

RecordError set_duration(TreeData &tree, TransactionNode &transaction, std::chrono::microseconds duration) noexcept {
    if (!on_owner_loop(tree)) {
        return RecordError::WrongEventLoop;
    }
    if (transaction.message.completed) {
        return RecordError::Completed;
    }
    if (duration.count() < 0) {
        return RecordError::InvalidArgument;
    }
    transaction.duration = duration;
    transaction.explicit_duration = true;
    return RecordError::None;
}

RecordError complete(TreeData &tree, MessageNode &message) noexcept {
    if (!on_owner_loop(tree)) {
        return RecordError::WrongEventLoop;
    }
    if (message.completed) {
        return RecordError::None;
    }
    if (message.kind == MessageKind::Transaction) {
        TransactionNode *transaction = as_transaction(&message);
        if (!transaction->explicit_duration) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(event::EventLoop::current().now() -
                                                                                 transaction->duration_start);
            transaction->duration = std::max(elapsed, std::chrono::microseconds::zero());
        }
    }
    message.completed = true;
    FIBER_ASSERT(tree.open_messages > 0);
    --tree.open_messages;
    if (message.status.view() != status::Success) {
        tree.has_problem = true;
    }
    return RecordError::None;
}

void complete_incomplete(TreeData &tree, MessageNode &message) noexcept {
    if (message.completed) {
        return;
    }
    message.status = {status::Incomplete.data(), status::Incomplete.size()};
    (void) complete(tree, message);
}

void retain(TreeData &tree) noexcept {
    FIBER_ASSERT(on_owner_loop(tree));
    FIBER_ASSERT(tree.refs < std::numeric_limits<std::size_t>::max());
    ++tree.refs;
}

void release(TreeData *tree) noexcept {
    if (!tree) {
        return;
    }
    FIBER_ASSERT(on_owner_loop(*tree));
    FIBER_ASSERT(tree->refs > 0);
    --tree->refs;
    if (tree->refs == 0) {
        delete tree;
    }
}

bool ready(const TreeData &tree) noexcept { return tree.root && tree.root->completed && tree.open_messages == 0; }

TransactionNode *as_transaction(MessageNode *message) noexcept {
    FIBER_ASSERT(message && message->kind == MessageKind::Transaction);
    return reinterpret_cast<TransactionNode *>(message);
}

const TransactionNode *as_transaction(const MessageNode *message) noexcept {
    FIBER_ASSERT(message && message->kind == MessageKind::Transaction);
    return reinterpret_cast<const TransactionNode *>(message);
}

} // namespace fiber::cat::detail
