#include <fiber/cat/Event.h>

#include <utility>

#include "CatInternal.h"

namespace fiber::cat {

Event::Event(Event &&other) noexcept : tree_(std::exchange(other.tree_, nullptr)), node_(other.node_) {
    other.node_ = nullptr;
}

Event &Event::operator=(Event &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    tree_ = std::exchange(other.tree_, nullptr);
    node_ = other.node_;
    other.node_ = nullptr;
    return *this;
}

Event::~Event() { reset(); }

std::expected<Event, RecordError> Event::create_root(std::string_view type, std::string_view name,
                                                     RecordLimits limits) noexcept {
    auto created = detail::create_event_tree(type, name, limits);
    if (!created) {
        return std::unexpected(created.error());
    }
    return Event(*created, (*created)->root);
}

bool Event::tree_ready() const noexcept { return tree_ && detail::ready(*tree_); }

bool Event::tree_has_problem() const noexcept { return tree_ && tree_->has_problem; }

RecordError Event::add_data(std::string_view data) noexcept {
    return tree_ ? detail::add_data(*tree_, *node_, data) : RecordError::InvalidArgument;
}

RecordError Event::add_data(std::string_view key, std::string_view value) noexcept {
    return tree_ ? detail::add_data(*tree_, *node_, key, value) : RecordError::InvalidArgument;
}

RecordError Event::set_status(std::string_view status_value) noexcept {
    return tree_ ? detail::set_status(*tree_, *node_, status_value) : RecordError::InvalidArgument;
}

RecordError Event::set_timestamp(std::uint64_t timestamp_millis) noexcept {
    return tree_ ? detail::set_timestamp(*tree_, *node_, timestamp_millis) : RecordError::InvalidArgument;
}

RecordError Event::complete() noexcept {
    return tree_ ? detail::complete(*tree_, *node_) : RecordError::InvalidArgument;
}

RecordError Event::complete(std::string_view status_value) noexcept {
    RecordError result = set_status(status_value);
    if (result != RecordError::None) {
        return result;
    }
    return complete();
}

void Event::reset() noexcept {
    if (!tree_) {
        return;
    }
    detail::complete_incomplete(*tree_, *node_);
    detail::release(tree_);
    tree_ = nullptr;
    node_ = nullptr;
}

} // namespace fiber::cat
