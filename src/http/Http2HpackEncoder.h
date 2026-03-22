#ifndef FIBER_HTTP_HTTP2_HPACK_ENCODER_H
#define FIBER_HTTP_HTTP2_HPACK_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "Http2HpackEncodeCatalog.h"
#include "Http2HpackEncodeTable.h"

namespace fiber::http {

class Http2HpackEncoder : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        const Http2HpackEncodeCatalog *catalog = nullptr;
        std::uint32_t max_dynamic_table_size = 4096;
        std::uint32_t max_string_size = 64 * 1024;
        std::size_t huffman_threshold = 16;
        std::size_t buffer_chunk_size = 512;
    };

    explicit Http2HpackEncoder(Options options) noexcept;

    [[nodiscard]] bool init() noexcept;
    void release() noexcept;

    void update_max_dynamic_table_size(std::uint32_t size) noexcept;

    [[nodiscard]] common::IoErr begin_block() noexcept;
    [[nodiscard]] common::IoErr encode_status(int status_code) noexcept;
    [[nodiscard]] common::IoErr encode_field(std::string_view name, std::uint64_t name_hash,
                                             std::string_view value) noexcept;
    [[nodiscard]] common::IoErr finish_block(mem::IoBufChain &out) noexcept;

private:
    enum class LiteralMode : std::uint8_t {
        IncrementalIndexing,
        WithoutIndexing,
    };

    [[nodiscard]] common::IoErr append_indexed(std::uint32_t index) noexcept;
    [[nodiscard]] common::IoErr append_table_size_update(std::uint32_t size) noexcept;
    [[nodiscard]] common::IoErr append_literal(std::uint32_t name_index, std::string_view name,
                                               std::string_view value, LiteralMode mode) noexcept;
    [[nodiscard]] common::IoErr append_string(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr append_integer(std::uint8_t first_byte_mask, std::uint8_t prefix_bits,
                                               std::uint32_t value) noexcept;
    [[nodiscard]] common::IoErr append_bytes(const std::uint8_t *data, std::size_t len) noexcept;
    [[nodiscard]] common::IoErr append_byte(std::uint8_t byte) noexcept;
    [[nodiscard]] common::IoErr ensure_tailroom(std::size_t min_bytes) noexcept;
    [[nodiscard]] bool should_huffman_encode(std::string_view value) const noexcept;
    [[nodiscard]] bool can_incrementally_index(const Http2HpackEncodeCatalog::EntryView &entry) const noexcept;
    [[nodiscard]] bool resolve_name_index(std::string_view name, std::uint64_t name_hash,
                                          std::uint32_t &index) const noexcept;

    const Options options_;
    Http2HpackEncodeTable table_;
    mem::IoBufChain block_;
    mem::IoBuf *tail_ = nullptr;
    bool block_open_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_ENCODER_H
