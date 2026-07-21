#include <fiber/cat/Message.h>

#include "CatInternal.h"

namespace fiber::cat {

DataEntryView DataIterator::operator*() const noexcept {
    if (!entry_) {
        return {};
    }
    if (entry_->key_value) {
        return {.key = entry_->key.view(), .value = entry_->value.view(), .key_value = true};
    }
    return {.value = entry_->key.view()};
}

DataIterator &DataIterator::operator++() noexcept {
    if (entry_) {
        entry_ = entry_->next;
    }
    return *this;
}

DataIterator DataIterator::operator++(int) noexcept {
    DataIterator previous = *this;
    ++*this;
    return previous;
}

MessageView ChildIterator::operator*() const noexcept { return MessageView(node_); }

ChildIterator &ChildIterator::operator++() noexcept {
    if (node_) {
        node_ = node_->next_sibling;
    }
    return *this;
}

ChildIterator ChildIterator::operator++(int) noexcept {
    ChildIterator previous = *this;
    ++*this;
    return previous;
}

MessageKind MessageView::kind() const noexcept { return node_ ? node_->kind : MessageKind::Event; }

std::string_view MessageView::type() const noexcept { return node_ ? node_->type.view() : std::string_view{}; }

std::string_view MessageView::name() const noexcept { return node_ ? node_->name.view() : std::string_view{}; }

std::string_view MessageView::status() const noexcept { return node_ ? node_->status.view() : std::string_view{}; }

std::uint64_t MessageView::timestamp_millis() const noexcept { return node_ ? node_->timestamp_millis : 0; }

bool MessageView::completed() const noexcept { return node_ && node_->completed; }

bool MessageView::success() const noexcept { return node_ && node_->status.view() == status::Success; }

std::chrono::microseconds MessageView::duration() const noexcept {
    if (!node_ || node_->kind != MessageKind::Transaction) {
        return {};
    }
    return detail::as_transaction(node_)->duration;
}

std::size_t MessageView::child_count() const noexcept {
    if (!node_ || node_->kind != MessageKind::Transaction) {
        return 0;
    }
    return detail::as_transaction(node_)->child_count;
}

std::size_t MessageView::data_size() const noexcept { return node_ ? node_->data_size : 0; }

DataRange MessageView::data() const noexcept { return DataRange(node_ ? node_->data_head : nullptr); }

ChildRange MessageView::children() const noexcept {
    if (!node_ || node_->kind != MessageKind::Transaction) {
        return ChildRange(nullptr);
    }
    return ChildRange(detail::as_transaction(node_)->children_head);
}

} // namespace fiber::cat
