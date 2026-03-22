#include "Http2HpackEncoder.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "HttpHeaderHash.h"
#include "Http2HpackHuffman.h"
#include "Http2HpackStaticTable.h"

namespace fiber::http {

namespace {

constexpr std::size_t kIntegerScratchCap = 8;
constexpr std::string_view kStatusName = ":status";
constexpr std::uint64_t kStatusNameHash = http_header_name_hash(kStatusName);

[[nodiscard]] std::string_view common_status_value(int status_code) noexcept {
    switch (status_code) {
        case 100:
            return "100";
        case 101:
            return "101";
        case 103:
            return "103";
        case 200:
            return "200";
        case 201:
            return "201";
        case 202:
            return "202";
        case 204:
            return "204";
        case 206:
            return "206";
        case 301:
            return "301";
        case 302:
            return "302";
        case 304:
            return "304";
        case 307:
            return "307";
        case 308:
            return "308";
        case 400:
            return "400";
        case 401:
            return "401";
        case 403:
            return "403";
        case 404:
            return "404";
        case 409:
            return "409";
        case 410:
            return "410";
        case 412:
            return "412";
        case 429:
            return "429";
        case 500:
            return "500";
        case 501:
            return "501";
        case 502:
            return "502";
        case 503:
            return "503";
        case 504:
            return "504";
        default:
            return {};
    }
}

[[nodiscard]] std::string_view format_status_value(int status_code, std::array<char, 3> &scratch) noexcept {
    if (status_code < 100 || status_code > 999) {
        return {};
    }
    scratch[0] = static_cast<char>('0' + ((status_code / 100) % 10));
    scratch[1] = static_cast<char>('0' + ((status_code / 10) % 10));
    scratch[2] = static_cast<char>('0' + (status_code % 10));
    return {scratch.data(), scratch.size()};
}

} // namespace

Http2HpackEncoder::Http2HpackEncoder(Options options) noexcept : options_(options) {}

bool Http2HpackEncoder::init() noexcept {
    release();
    if (options_.catalog == nullptr) {
        return false;
    }
    return table_.init(*options_.catalog, options_.max_dynamic_table_size);
}

void Http2HpackEncoder::release() noexcept {
    block_.clear();
    tail_ = nullptr;
    block_open_ = false;
    table_.release();
}

void Http2HpackEncoder::update_max_dynamic_table_size(std::uint32_t size) noexcept {
    table_.update_max_dynamic_table_size(size);
}

common::IoErr Http2HpackEncoder::begin_block() noexcept {
    if (block_open_) {
        return common::IoErr::Invalid;
    }
    if (options_.catalog == nullptr || table_.catalog() == nullptr) {
        return common::IoErr::Invalid;
    }

    block_.clear();
    tail_ = nullptr;
    block_open_ = true;

    if (table_.has_pending_table_size_update()) {
        common::IoErr err = append_table_size_update(table_.pending_dynamic_table_size());
        if (err != common::IoErr::None) {
            block_open_ = false;
            block_.clear();
            tail_ = nullptr;
            return err;
        }
        table_.acknowledge_table_size_update();
    }
    return common::IoErr::None;
}

common::IoErr Http2HpackEncoder::encode_status(int status_code) noexcept {
    std::string_view value = common_status_value(status_code);
    std::array<char, 3> scratch{};
    if (value.empty()) {
        value = format_status_value(status_code, scratch);
    }
    if (value.empty()) {
        return common::IoErr::Invalid;
    }
    return encode_field(kStatusName, kStatusNameHash, value);
}

common::IoErr Http2HpackEncoder::encode_field(std::string_view name, std::uint64_t name_hash,
                                              std::string_view value) noexcept {
    if (!block_open_ || options_.catalog == nullptr) {
        return common::IoErr::Invalid;
    }
    if (name.size() > options_.max_string_size || value.size() > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    const Http2HpackEncodeCatalog::FindResult result = options_.catalog->find(name, name_hash, value);
    if (result.entry != nullptr && result.exact) {
        std::uint32_t index = 0;
        if (table_.resolve_index(result.entry, index)) {
            return append_indexed(index);
        }

        if (can_incrementally_index(*result.entry)) {
            std::uint32_t name_index = 0;
            (void) resolve_name_index(name, name_hash, name_index);
            common::IoErr err = append_literal(name_index, name, value, LiteralMode::IncrementalIndexing);
            if (err != common::IoErr::None) {
                return err;
            }
            const auto activate_result = table_.activate(result.entry);
            if (activate_result == Http2HpackEncodeTable::ActivateResult::Rejected ||
                activate_result == Http2HpackEncodeTable::ActivateResult::InvalidId) {
                return common::IoErr::Invalid;
            }
            return common::IoErr::None;
        }
    }

    std::uint32_t name_index = 0;
    if (resolve_name_index(name, name_hash, name_index)) {
        return append_literal(name_index, {}, value, LiteralMode::WithoutIndexing);
    }
    return append_literal(0, name, value, LiteralMode::WithoutIndexing);
}

common::IoErr Http2HpackEncoder::finish_block(mem::IoBufChain &out) noexcept {
    if (!block_open_) {
        return common::IoErr::Invalid;
    }
    out = std::move(block_);
    tail_ = nullptr;
    block_open_ = false;
    return common::IoErr::None;
}

common::IoErr Http2HpackEncoder::append_indexed(std::uint32_t index) noexcept {
    return append_integer(0x80U, 7, index);
}

common::IoErr Http2HpackEncoder::append_table_size_update(std::uint32_t size) noexcept {
    return append_integer(0x20U, 5, size);
}

common::IoErr Http2HpackEncoder::append_literal(std::uint32_t name_index, std::string_view name,
                                                std::string_view value, LiteralMode mode) noexcept {
    const bool new_name = name_index == 0;
    const std::uint8_t first_mask = mode == LiteralMode::IncrementalIndexing ? 0x40U : 0x00U;
    const std::uint8_t prefix_bits = mode == LiteralMode::IncrementalIndexing ? 6U : 4U;
    common::IoErr err = append_integer(first_mask, prefix_bits, name_index);
    if (err != common::IoErr::None) {
        return err;
    }
    if (new_name) {
        err = append_string(name);
        if (err != common::IoErr::None) {
            return err;
        }
    }
    return append_string(value);
}

common::IoErr Http2HpackEncoder::append_string(std::string_view value) noexcept {
    if (value.size() > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    if (!should_huffman_encode(value)) {
        common::IoErr err = append_integer(0x00U, 7, static_cast<std::uint32_t>(value.size()));
        if (err != common::IoErr::None) {
            return err;
        }
        return append_bytes(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    }

    const std::size_t encoded_len =
        http2_huffman_encoded_length(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    if (encoded_len > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = append_integer(0x80U, 7, static_cast<std::uint32_t>(encoded_len));
    if (err != common::IoErr::None) {
        return err;
    }
    err = ensure_tailroom(encoded_len);
    if (err != common::IoErr::None) {
        return err;
    }
    std::uint8_t *dst = tail_->writable_data();
    const std::size_t written =
        http2_huffman_encode_exact(reinterpret_cast<const std::uint8_t *>(value.data()), value.size(), dst);
    if (written != encoded_len) {
        return common::IoErr::Invalid;
    }
    block_.commit(written);
    tail_ = block_.first_writable();
    return common::IoErr::None;
}

common::IoErr Http2HpackEncoder::append_integer(std::uint8_t first_byte_mask, std::uint8_t prefix_bits,
                                                std::uint32_t value) noexcept {
    std::array<std::uint8_t, kIntegerScratchCap> scratch{};
    const std::uint32_t prefix_max = (1U << prefix_bits) - 1U;
    std::size_t used = 0;
    if (value < prefix_max) {
        scratch[used++] = static_cast<std::uint8_t>(first_byte_mask | value);
        return append_bytes(scratch.data(), used);
    }

    scratch[used++] = static_cast<std::uint8_t>(first_byte_mask | prefix_max);
    value -= prefix_max;
    while (value >= 128U) {
        scratch[used++] = static_cast<std::uint8_t>((value & 0x7fU) | 0x80U);
        value >>= 7U;
    }
    scratch[used++] = static_cast<std::uint8_t>(value);
    return append_bytes(scratch.data(), used);
}

common::IoErr Http2HpackEncoder::append_bytes(const std::uint8_t *data, std::size_t len) noexcept {
    if (len == 0) {
        return common::IoErr::None;
    }
    common::IoErr err = ensure_tailroom(len);
    if (err != common::IoErr::None) {
        return err;
    }
    std::memcpy(tail_->writable_data(), data, len);
    block_.commit(len);
    tail_ = block_.first_writable();
    return common::IoErr::None;
}

common::IoErr Http2HpackEncoder::append_byte(std::uint8_t byte) noexcept {
    return append_bytes(&byte, 1);
}

common::IoErr Http2HpackEncoder::ensure_tailroom(std::size_t min_bytes) noexcept {
    if (tail_ != nullptr && tail_->writable() >= min_bytes) {
        return common::IoErr::None;
    }

    const std::size_t cap = std::max(options_.buffer_chunk_size, min_bytes);
    mem::IoBuf buf = mem::IoBuf::allocate(cap);
    if (!buf.valid()) {
        return common::IoErr::NoMem;
    }
    if (!block_.append(std::move(buf))) {
        return common::IoErr::NoMem;
    }
    tail_ = block_.first_writable();
    return tail_ != nullptr ? common::IoErr::None : common::IoErr::NoMem;
}

bool Http2HpackEncoder::should_huffman_encode(std::string_view value) const noexcept {
    return value.size() >= options_.huffman_threshold;
}

bool Http2HpackEncoder::can_incrementally_index(const Http2HpackEncodeCatalog::EntryView &entry) const noexcept {
    return entry.kind == Http2HpackEncodeCatalog::EntryKind::Policy &&
           entry.entry_size <= table_.max_dynamic_table_size() &&
           table_.max_dynamic_table_size() != 0;
}

bool Http2HpackEncoder::resolve_name_index(std::string_view name, std::uint64_t name_hash,
                                           std::uint32_t &index) const noexcept {
    index = 0;
    if (Http2HpackStaticTable::find_name(name, name_hash, index)) {
        return true;
    }
    if (options_.catalog == nullptr) {
        return false;
    }
    const Http2HpackEncodeCatalog::FindResult name_result = options_.catalog->find(name, name_hash, {});
    if (name_result.entry == nullptr) {
        return false;
    }
    return table_.resolve_index(name_result.entry, index);
}

} // namespace fiber::http
