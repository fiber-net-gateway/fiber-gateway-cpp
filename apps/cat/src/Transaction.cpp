#include <fiber/cat/Transaction.h>

#include <utility>

#include "CatInternal.h"

namespace fiber::cat {

Transaction::Transaction(Transaction &&other) noexcept :
    tree_(std::exchange(other.tree_, nullptr)), node_(other.node_) {
    other.node_ = nullptr;
}

Transaction &Transaction::operator=(Transaction &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    tree_ = std::exchange(other.tree_, nullptr);
    node_ = other.node_;
    other.node_ = nullptr;
    return *this;
}

Transaction::~Transaction() { reset(); }

std::expected<Transaction, RecordError> Transaction::create_root(std::string_view type, std::string_view name,
                                                                 RecordLimits limits) noexcept {
    auto created = detail::create_transaction_tree(type, name, limits);
    if (!created) {
        return std::unexpected(created.error());
    }
    return Transaction(*created, (*created)->root);
}

bool Transaction::tree_ready() const noexcept { return tree_ && detail::ready(*tree_); }

bool Transaction::tree_has_problem() const noexcept { return tree_ && tree_->has_problem; }

std::expected<Transaction, RecordError> Transaction::start_transaction(std::string_view type,
                                                                       std::string_view name) noexcept {
    if (!tree_) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    auto child = detail::create_transaction(*tree_, detail::as_transaction(node_), type, name);
    if (!child) {
        return std::unexpected(child.error());
    }
    detail::retain(*tree_);
    return Transaction(tree_, &(*child)->message);
}

std::expected<Event, RecordError> Transaction::start_event(std::string_view type, std::string_view name) noexcept {
    if (!tree_) {
        return std::unexpected(RecordError::InvalidArgument);
    }
    auto child = detail::create_event(*tree_, detail::as_transaction(node_), type, name);
    if (!child) {
        return std::unexpected(child.error());
    }
    detail::retain(*tree_);
    return Event(tree_, *child);
}

RecordError Transaction::log_event(std::string_view type, std::string_view name, std::string_view status_value,
                                   std::string_view data) noexcept {
    auto created = start_event(type, name);
    if (!created) {
        return created.error();
    }
    Event event = std::move(*created);
    RecordError result = RecordError::None;
    if (status_value != status::Success) {
        result = event.set_status(status_value);
    }
    if (!data.empty()) {
        const RecordError data_result = event.add_data(data);
        if (result == RecordError::None) {
            result = data_result;
        }
    }
    const RecordError complete_result = event.complete();
    return result != RecordError::None ? result : complete_result;
}

RecordError Transaction::add_data(std::string_view data) noexcept {
    return tree_ ? detail::add_data(*tree_, *node_, data) : RecordError::InvalidArgument;
}

RecordError Transaction::add_data(std::string_view key, std::string_view value) noexcept {
    return tree_ ? detail::add_data(*tree_, *node_, key, value) : RecordError::InvalidArgument;
}

RecordError Transaction::set_status(std::string_view status_value) noexcept {
    return tree_ ? detail::set_status(*tree_, *node_, status_value) : RecordError::InvalidArgument;
}

RecordError Transaction::set_timestamp(std::uint64_t timestamp_millis) noexcept {
    return tree_ ? detail::set_timestamp(*tree_, *node_, timestamp_millis) : RecordError::InvalidArgument;
}

RecordError Transaction::set_duration(std::chrono::microseconds duration) noexcept {
    return tree_ ? detail::set_duration(*tree_, *detail::as_transaction(node_), duration)
                 : RecordError::InvalidArgument;
}

RecordError Transaction::complete() noexcept {
    return tree_ ? detail::complete(*tree_, *node_) : RecordError::InvalidArgument;
}

RecordError Transaction::complete(std::string_view status_value) noexcept {
    RecordError result = set_status(status_value);
    if (result != RecordError::None) {
        return result;
    }
    return complete();
}

void Transaction::reset() noexcept {
    if (!tree_) {
        return;
    }
    detail::complete_incomplete(*tree_, *node_);
    detail::release(tree_);
    tree_ = nullptr;
    node_ = nullptr;
}

} // namespace fiber::cat
