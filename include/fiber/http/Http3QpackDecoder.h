#ifndef FIBER_HTTP_HTTP3_QPACK_DECODER_H
#define FIBER_HTTP_HTTP3_QPACK_DECODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http3QpackStaticTable.h"
#include "Http3QpackTableEntryView.h"

namespace fiber::http {

class Http3QpackDecoder : public common::NonCopyable, public common::NonMovable {
public:
    using TableEntryView = Http3QpackTableEntryView;

    struct Ops {
        common::IoErr (*on_indexed_field)(void *ctx, TableEntryView entry) noexcept = nullptr;
        common::IoErr (*on_indexed_name)(void *ctx, std::string_view name, std::uint64_t name_hash) noexcept = nullptr;
        common::IoErr (*on_name_raw)(void *ctx, const std::uint8_t *data, std::size_t len) noexcept = nullptr;
        common::IoErr (*on_name_huffman)(void *ctx, const std::uint8_t *data, std::size_t len) noexcept = nullptr;
        common::IoErr (*on_value_raw)(void *ctx, const std::uint8_t *data, std::size_t len) noexcept = nullptr;
        common::IoErr (*on_value_huffman)(void *ctx, const std::uint8_t *data, std::size_t len) noexcept = nullptr;
    };

    struct Sink {
        void *ctx = nullptr;
        const Ops *ops = nullptr;
    };

    Http3QpackDecoder() noexcept = default;

    [[nodiscard]] bool init(std::uint32_t max_string_size = 64 * 1024U) noexcept;
    void release() noexcept;
    void begin_block(void *ctx, const Ops *ops) noexcept;
    [[nodiscard]] common::IoErr decode(const std::uint8_t *data, std::size_t len, bool end_block) noexcept;

private:
    enum class State : std::uint8_t {
        PrefixRequiredInsertCount,
        PrefixRequiredInsertCountCont,
        PrefixBaseFirst,
        PrefixBaseCont,
        Ready,
        IndexedCont,
        LiteralNameIndexCont,
        LiteralNameLenCont,
        LiteralNameData,
        LiteralValueLenFirst,
        LiteralValueLenCont,
        LiteralValueData,
    };

    [[nodiscard]] common::IoErr parse_required_insert_count(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr parse_base_first(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr parse_ready(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr parse_integer_cont(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr parse_value_length_first(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr parse_string_data(const std::uint8_t *&pos, const std::uint8_t *end) noexcept;
    [[nodiscard]] common::IoErr handle_indexed_field(std::uint64_t index) noexcept;
    [[nodiscard]] common::IoErr handle_indexed_name(std::uint64_t index) noexcept;
    [[nodiscard]] common::IoErr handle_string_complete() noexcept;
    [[nodiscard]] common::IoErr handle_literal_value_start() noexcept;
    [[nodiscard]] common::IoErr prepare_string_accumulator(std::uint64_t string_length, State data_state) noexcept;
    [[nodiscard]] bool resolve_static_index(std::uint64_t index, TableEntryView &entry) const noexcept;
    [[nodiscard]] bool ensure_scratch_capacity(std::size_t size) noexcept;
    void reset_block_state() noexcept;
    void reset_string_accumulator() noexcept;
    void finish_literal_field() noexcept;

    std::unique_ptr<std::uint8_t[]> scratch_;
    std::uint32_t scratch_cap_ = 0;
    std::uint32_t max_string_size_ = 64 * 1024U;
    void *ctx_ = nullptr;
    const Ops *ops_ = nullptr;
    State state_ = State::PrefixRequiredInsertCount;
    bool string_huffman_ = false;
    bool have_literal_name_ = false;
    std::uint64_t integer_value_ = 0;
    std::uint64_t integer_prefix_max_ = 0;
    std::uint8_t integer_shift_ = 0;
    std::uint32_t string_length_ = 0;
    std::uint32_t string_received_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_QPACK_DECODER_H
