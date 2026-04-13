#ifndef FIBER_HTTP_HTTP2_HPACK_ENCODE_TABLE_H
#define FIBER_HTTP_HTTP2_HPACK_ENCODE_TABLE_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2HpackEncodeCatalog.h"

namespace fiber::http {

class Http2HpackEncodeTable : public common::NonCopyable, public common::NonMovable {
public:
    static constexpr std::uint32_t kInvalidSlot = 0xffffffffU;

    enum class ActivateResult : std::uint8_t {
        Activated,
        AlreadyActive,
        StaticEntry,
        InvalidId,
        Rejected,
    };

    Http2HpackEncodeTable() noexcept = default;

    [[nodiscard]] bool init(const Http2HpackEncodeCatalog &catalog, std::uint32_t max_dynamic_table_size) noexcept;
    void release() noexcept;

    void update_max_dynamic_table_size(std::uint32_t size) noexcept;
    [[nodiscard]] bool has_pending_table_size_update() const noexcept { return pending_table_size_update_; }
    [[nodiscard]] std::uint32_t pending_dynamic_table_size() const noexcept { return target_dynamic_table_size_; }
    void acknowledge_table_size_update() noexcept;

    [[nodiscard]] ActivateResult activate(const Http2HpackEncodeCatalog::EntryView *entry) noexcept;
    [[nodiscard]] bool is_active(const Http2HpackEncodeCatalog::EntryView *entry) const noexcept;
    [[nodiscard]] bool resolve_index(const Http2HpackEncodeCatalog::EntryView *entry,
                                     std::uint32_t &hpack_index) const noexcept;

    [[nodiscard]] std::uint32_t current_dynamic_size() const noexcept { return current_dynamic_size_; }
    [[nodiscard]] std::uint32_t max_dynamic_table_size() const noexcept { return target_dynamic_table_size_; }
    [[nodiscard]] std::uint32_t active_count() const noexcept { return active_count_; }
    [[nodiscard]] const Http2HpackEncodeCatalog *catalog() const noexcept { return catalog_; }

private:
    struct PolicyState {
        bool active = false;
        std::uint32_t prev = kInvalidSlot;
        std::uint32_t next = kInvalidSlot;
    };

    [[nodiscard]] bool owns_entry(const Http2HpackEncodeCatalog::EntryView *entry) const noexcept;
    void unlink_slot(std::uint32_t slot) noexcept;
    void link_slot_as_newest(std::uint32_t slot) noexcept;
    void evict_oldest() noexcept;

    const Http2HpackEncodeCatalog *catalog_ = nullptr;
    std::unique_ptr<PolicyState[]> policy_state_;
    std::uint32_t policy_count_ = 0;
    std::uint32_t current_dynamic_size_ = 0;
    std::uint32_t target_dynamic_table_size_ = 0;
    std::uint32_t signaled_dynamic_table_size_ = 0;
    std::uint32_t newest_slot_ = kInvalidSlot;
    std::uint32_t oldest_slot_ = kInvalidSlot;
    std::uint32_t active_count_ = 0;
    bool pending_table_size_update_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_ENCODE_TABLE_H
