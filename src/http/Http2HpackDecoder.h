#ifndef FIBER_HTTP_HTTP2_HPACK_DECODER_H
#define FIBER_HTTP_HTTP2_HPACK_DECODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2HpackDynamicTable.h"
#include "Http2HpackStaticTable.h"
#include "Http2HpackTableEntryView.h"

namespace fiber::http {

class Http2HpackDecoder : public common::NonCopyable, public common::NonMovable {
public:
    using TableEntryView = Http2HpackTableEntryView;

    struct NameView {
        std::string_view name;
        std::uint64_t name_hash = 0;
    };

    struct Ops {
        common::IoErr (*on_indexed_field)(void *ctx, TableEntryView entry) noexcept = nullptr;
        common::IoErr (*on_indexed_name)(void *ctx, std::string_view name, std::uint64_t name_hash) noexcept = nullptr;
        common::IoErr (*on_name_raw)(void *ctx, const std::uint8_t *data, std::size_t len,
                                     NameView &out) noexcept = nullptr;
        common::IoErr (*on_name_huffman)(void *ctx, const std::uint8_t *data, std::size_t len,
                                         NameView &out) noexcept = nullptr;
        common::IoErr (*on_value_raw)(void *ctx, const std::uint8_t *data, std::size_t len,
                                      std::string_view &out) noexcept = nullptr;
        common::IoErr (*on_value_huffman)(void *ctx, const std::uint8_t *data, std::size_t len,
                                          std::string_view &out) noexcept = nullptr;
    };

    struct Sink {
        void *ctx = nullptr;
        const Ops *ops = nullptr;
    };

    Http2HpackDecoder() noexcept = default;

    [[nodiscard]] bool init(std::uint32_t max_dynamic_table_size = 4096U) noexcept;
    void release() noexcept;
    void begin_block(void *ctx, const Ops *ops) noexcept;
    [[nodiscard]] common::IoErr decode(const std::uint8_t *data, std::size_t len, bool end_block) noexcept;

private:
    enum class State : std::uint8_t {
        Ready,
        IndexedCont,
        LiteralNameIndexCont,
        LiteralNameLenFirst,
        LiteralNameLenCont,
        LiteralNameData,
        LiteralValueLenFirst,
        LiteralValueLenCont,
        LiteralValueData,
        TableSizeUpdateCont,
    };

    enum class LiteralMode : std::uint8_t {
        IncrementalIndexing,
        WithoutIndexing,
        NeverIndexed,
    };

    [[nodiscard]] common::IoErr parse_ready(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr parse_integer_cont(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr parse_string_data(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr handle_indexed_field(std::uint32_t index) noexcept;
    [[nodiscard]] common::IoErr handle_indexed_name(std::uint32_t index) noexcept;
    [[nodiscard]] common::IoErr handle_string_complete() noexcept;
    [[nodiscard]] common::IoErr handle_literal_value_start() noexcept;
    [[nodiscard]] common::IoErr apply_table_size_update(std::uint32_t size) noexcept;
    [[nodiscard]] bool resolve_index(std::uint32_t index, TableEntryView &entry) const noexcept;
    [[nodiscard]] bool ensure_scratch_capacity(std::size_t size) noexcept;
    [[nodiscard]] bool ensure_name_storage_capacity(std::size_t size) noexcept;
    void reset_string_accumulator() noexcept;
    void finish_literal_field() noexcept;

    Http2HpackDynamicTable dynamic_table_;
    std::unique_ptr<std::uint8_t[]> scratch_;
    std::unique_ptr<char[]> name_storage_;
    std::uint32_t scratch_cap_ = 0;
    std::uint32_t name_storage_cap_ = 0;
    std::uint32_t max_dynamic_table_size_ = 4096U;
    void *ctx_ = nullptr;
    const Ops *ops_ = nullptr;
    State state_ = State::Ready;
    LiteralMode literal_mode_ = LiteralMode::WithoutIndexing;
    bool string_huffman_ = false;
    bool allow_table_size_update_ = true;
    bool have_literal_name_ = false;
    std::uint64_t current_name_hash_ = 0;
    std::string_view current_name_;
    std::uint32_t integer_value_ = 0;
    std::uint32_t integer_prefix_max_ = 0;
    std::uint32_t integer_shift_ = 0;
    std::uint32_t string_length_ = 0;
    std::uint32_t string_received_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_DECODER_H
