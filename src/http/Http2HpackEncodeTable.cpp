#include "Http2HpackEncodeTable.h"

#include <new>

namespace fiber::http {

bool Http2HpackEncodeTable::init(const Http2HpackEncodeCatalog &catalog,
                                 std::uint32_t max_dynamic_table_size) noexcept {
    release();

    std::unique_ptr<PolicyState[]> policy_state;
    if (catalog.policy_count() != 0) {
        policy_state.reset(new (std::nothrow) PolicyState[catalog.policy_count()]);
        if (!policy_state) {
            return false;
        }
    }

    catalog_ = &catalog;
    policy_state_ = std::move(policy_state);
    policy_count_ = catalog.policy_count();
    current_dynamic_size_ = 0;
    target_dynamic_table_size_ = max_dynamic_table_size;
    signaled_dynamic_table_size_ = max_dynamic_table_size;
    newest_slot_ = kInvalidSlot;
    oldest_slot_ = kInvalidSlot;
    active_count_ = 0;
    pending_table_size_update_ = false;
    return true;
}

void Http2HpackEncodeTable::release() noexcept {
    catalog_ = nullptr;
    policy_state_.reset();
    policy_count_ = 0;
    current_dynamic_size_ = 0;
    target_dynamic_table_size_ = 0;
    signaled_dynamic_table_size_ = 0;
    newest_slot_ = kInvalidSlot;
    oldest_slot_ = kInvalidSlot;
    active_count_ = 0;
    pending_table_size_update_ = false;
}

void Http2HpackEncodeTable::update_max_dynamic_table_size(std::uint32_t size) noexcept {
    target_dynamic_table_size_ = size;
    pending_table_size_update_ = (target_dynamic_table_size_ != signaled_dynamic_table_size_);

    while (current_dynamic_size_ > target_dynamic_table_size_ && oldest_slot_ != kInvalidSlot) {
        evict_oldest();
    }
}

void Http2HpackEncodeTable::acknowledge_table_size_update() noexcept {
    signaled_dynamic_table_size_ = target_dynamic_table_size_;
    pending_table_size_update_ = false;
}

Http2HpackEncodeTable::ActivateResult
Http2HpackEncodeTable::activate(const Http2HpackEncodeCatalog::EntryView *entry) noexcept {
    if (!owns_entry(entry)) {
        return ActivateResult::InvalidId;
    }
    if (catalog_->is_static_entry(entry)) {
        return ActivateResult::StaticEntry;
    }
    const std::uint32_t slot = entry->policy_slot;
    PolicyState &state = policy_state_[slot];
    if (state.active) {
        return ActivateResult::AlreadyActive;
    }
    if (entry->kind != Http2HpackEncodeCatalog::EntryKind::Policy || entry->entry_size > target_dynamic_table_size_ ||
        target_dynamic_table_size_ == 0) {
        return ActivateResult::Rejected;
    }

    while (current_dynamic_size_ + entry->entry_size > target_dynamic_table_size_ && oldest_slot_ != kInvalidSlot) {
        evict_oldest();
    }
    if (current_dynamic_size_ + entry->entry_size > target_dynamic_table_size_) {
        return ActivateResult::Rejected;
    }

    state.active = true;
    state.prev = kInvalidSlot;
    state.next = kInvalidSlot;
    link_slot_as_newest(slot);
    current_dynamic_size_ += entry->entry_size;
    ++active_count_;
    return ActivateResult::Activated;
}

bool Http2HpackEncodeTable::is_active(const Http2HpackEncodeCatalog::EntryView *entry) const noexcept {
    if (!owns_entry(entry) || !catalog_->is_policy_entry(entry)) {
        return false;
    }
    return policy_state_[entry->policy_slot].active;
}

bool Http2HpackEncodeTable::resolve_index(const Http2HpackEncodeCatalog::EntryView *entry,
                                          std::uint32_t &hpack_index) const noexcept {
    hpack_index = 0;
    if (!owns_entry(entry)) {
        return false;
    }
    if (entry->kind == Http2HpackEncodeCatalog::EntryKind::Static) {
        hpack_index = entry->hpack_index;
        return hpack_index != 0;
    }
    if (!is_active(entry)) {
        return false;
    }

    const std::uint32_t target_slot = entry->policy_slot;
    std::uint32_t dynamic_index = 1;
    for (std::uint32_t slot = newest_slot_; slot != kInvalidSlot; slot = policy_state_[slot].next) {
        if (slot == target_slot) {
            hpack_index = catalog_->static_count() + dynamic_index;
            return true;
        }
        ++dynamic_index;
    }
    return false;
}

bool Http2HpackEncodeTable::owns_entry(const Http2HpackEncodeCatalog::EntryView *entry) const noexcept {
    return catalog_ != nullptr && entry != nullptr && catalog_->entries_ != nullptr &&
           entry >= catalog_->entries_.get() && entry < catalog_->entries_.get() + catalog_->entry_count_;
}

void Http2HpackEncodeTable::unlink_slot(std::uint32_t slot) noexcept {
    PolicyState &state = policy_state_[slot];
    if (state.prev != kInvalidSlot) {
        policy_state_[state.prev].next = state.next;
    } else {
        newest_slot_ = state.next;
    }
    if (state.next != kInvalidSlot) {
        policy_state_[state.next].prev = state.prev;
    } else {
        oldest_slot_ = state.prev;
    }
    state.prev = kInvalidSlot;
    state.next = kInvalidSlot;
}

void Http2HpackEncodeTable::link_slot_as_newest(std::uint32_t slot) noexcept {
    PolicyState &state = policy_state_[slot];
    state.prev = kInvalidSlot;
    state.next = newest_slot_;
    if (newest_slot_ != kInvalidSlot) {
        policy_state_[newest_slot_].prev = slot;
    } else {
        oldest_slot_ = slot;
    }
    newest_slot_ = slot;
}

void Http2HpackEncodeTable::evict_oldest() noexcept {
    if (oldest_slot_ == kInvalidSlot || !catalog_) {
        return;
    }

    const std::uint32_t slot = oldest_slot_;
    const Http2HpackEncodeCatalog::EntryView &entry = catalog_->entries_[catalog_->static_count() + slot];
    if (current_dynamic_size_ >= entry.entry_size) {
        current_dynamic_size_ -= entry.entry_size;
    } else {
        current_dynamic_size_ = 0;
    }

    unlink_slot(slot);
    PolicyState &state = policy_state_[slot];
    state.active = false;
    if (active_count_ != 0) {
        --active_count_;
    }
}

} // namespace fiber::http
