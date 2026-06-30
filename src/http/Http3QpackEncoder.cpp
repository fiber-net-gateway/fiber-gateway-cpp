#include "Http3QpackEncoder.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "../common/Assert.h"
#include "Http3QpackStaticTable.h"
#include "HttpHeaderHash.h"
#include "Huffman.h"

namespace fiber::http {

namespace {

constexpr std::size_t kIntegerScratchCap = 16;
constexpr std::string_view kStatusName = ":status";
constexpr std::string_view kMethodName = ":method";
constexpr std::string_view kSchemeName = ":scheme";
constexpr std::string_view kAuthorityName = ":authority";
constexpr std::string_view kPathName = ":path";
constexpr std::uint64_t kStatusNameHash = http_header_name_hash(kStatusName);
constexpr std::uint64_t kMethodNameHash = http_header_name_hash(kMethodName);
constexpr std::uint64_t kSchemeNameHash = http_header_name_hash(kSchemeName);
constexpr std::uint64_t kAuthorityNameHash = http_header_name_hash(kAuthorityName);
constexpr std::uint64_t kPathNameHash = http_header_name_hash(kPathName);

[[nodiscard]] std::string_view format_status_value(int status_code, std::array<char, 3> &scratch) noexcept {
    if (status_code < 100 || status_code > 999) {
        return {};
    }
    scratch[0] = static_cast<char>('0' + ((status_code / 100) % 10));
    scratch[1] = static_cast<char>('0' + ((status_code / 10) % 10));
    scratch[2] = static_cast<char>('0' + (status_code % 10));
    return {scratch.data(), scratch.size()};
}

[[nodiscard]] std::string_view common_method_value(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::MKCOL:
            return "MKCOL";
        case HttpMethod::Copy:
            return "COPY";
        case HttpMethod::Move:
            return "MOVE";
        case HttpMethod::Options:
            return "OPTIONS";
        case HttpMethod::PropFind:
            return "PROPFIND";
        case HttpMethod::PropPatch:
            return "PROPPATCH";
        case HttpMethod::Lock:
            return "LOCK";
        case HttpMethod::Unlock:
            return "UNLOCK";
        case HttpMethod::Patch:
            return "PATCH";
        case HttpMethod::Trace:
            return "TRACE";
        case HttpMethod::Connect:
            return "CONNECT";
        case HttpMethod::Unknown:
        default:
            return {};
    }
}

} // namespace

Http3QpackEncoder::Http3QpackEncoder(void *output_ctx, OutputOps output_ops) noexcept :
    Http3QpackEncoder(output_ctx, output_ops, Options{}) {}

Http3QpackEncoder::Http3QpackEncoder(void *output_ctx, OutputOps output_ops, Options options) noexcept :
    options_(options), output_ctx_(output_ctx), output_ops_(output_ops) {
    FIBER_ASSERT(output_ctx_ != nullptr);
    FIBER_ASSERT(output_ops_.acquire != nullptr);
    FIBER_ASSERT(output_ops_.commit != nullptr);
}

common::IoErr Http3QpackEncoder::encode_status(int status_code) noexcept {
    std::array<char, 3> scratch{};
    const std::string_view value = format_status_value(status_code, scratch);
    if (value.empty()) {
        return common::IoErr::Invalid;
    }
    return encode_field(kStatusName, kStatusNameHash, value);
}

common::IoErr Http3QpackEncoder::encode_method(HttpMethod method) noexcept {
    const std::string_view value = common_method_value(method);
    if (value.empty()) {
        return common::IoErr::Invalid;
    }
    return encode_field(kMethodName, kMethodNameHash, value);
}

common::IoErr Http3QpackEncoder::encode_scheme(std::string_view scheme) noexcept {
    return encode_field(kSchemeName, kSchemeNameHash, scheme);
}

common::IoErr Http3QpackEncoder::encode_authority(std::string_view authority) noexcept {
    return encode_field(kAuthorityName, kAuthorityNameHash, authority);
}

common::IoErr Http3QpackEncoder::encode_path(std::string_view path) noexcept {
    return encode_field(kPathName, kPathNameHash, path);
}

common::IoErr Http3QpackEncoder::encode_field(std::string_view name, std::uint64_t name_hash,
                                              std::string_view value) noexcept {
    FIBER_ASSERT(!finished_);
    if (name.empty() || name.size() > options_.max_string_size || value.size() > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = ensure_prefix();
    if (err != common::IoErr::None) {
        return err;
    }

    const std::uint64_t effective_name_hash = name_hash != 0 ? name_hash : http_header_name_hash(name);
    const Http3QpackStaticTable::FindResult result = Http3QpackStaticTable::find(name, effective_name_hash, value);
    if (result.kind == Http3QpackStaticTable::FindKind::EntryMatch) {
        return append_indexed(result.index);
    }
    if (result.kind == Http3QpackStaticTable::FindKind::NameMatch) {
        return append_literal_static_name(result.index, value);
    }
    return append_literal_name(name, value);
}

common::IoErr Http3QpackEncoder::finish() noexcept {
    FIBER_ASSERT(!finished_);
    common::IoErr err = ensure_prefix();
    if (err != common::IoErr::None) {
        return err;
    }
    finished_ = true;
    return common::IoErr::None;
}

common::IoErr Http3QpackEncoder::ensure_prefix() noexcept {
    if (prefix_written_) {
        return common::IoErr::None;
    }
    common::IoErr err = append_byte(0x00U);
    if (err != common::IoErr::None) {
        return err;
    }
    err = append_byte(0x00U);
    if (err != common::IoErr::None) {
        return err;
    }
    prefix_written_ = true;
    return common::IoErr::None;
}

common::IoErr Http3QpackEncoder::append_indexed(std::uint32_t index) noexcept {
    return append_integer(0xc0U, 6, index);
}

common::IoErr Http3QpackEncoder::append_literal_static_name(std::uint32_t name_index, std::string_view value) noexcept {
    common::IoErr err = append_integer(0x50U, 4, name_index);
    if (err != common::IoErr::None) {
        return err;
    }
    return append_string(value);
}

common::IoErr Http3QpackEncoder::append_literal_name(std::string_view name, std::string_view value) noexcept {
    common::IoErr err = append_literal_name_string(name);
    if (err != common::IoErr::None) {
        return err;
    }
    return append_string(value);
}

common::IoErr Http3QpackEncoder::append_string(std::string_view value) noexcept {
    return append_prefixed_string(value, 0x00U, 0x80U, 7);
}

common::IoErr Http3QpackEncoder::append_literal_name_string(std::string_view name) noexcept {
    return append_prefixed_string(name, 0x20U, 0x08U, 3);
}

common::IoErr Http3QpackEncoder::append_prefixed_string(std::string_view value, std::uint8_t first_byte_mask,
                                                        std::uint8_t huffman_mask, std::uint8_t prefix_bits) noexcept {
    if (value.size() > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    if (!should_huffman_encode(value)) {
        common::IoErr err = append_integer(first_byte_mask, prefix_bits, value.size());
        if (err != common::IoErr::None) {
            return err;
        }
        return append_bytes(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    }

    const std::size_t encoded_len =
            hpack_huffman_encoded_length(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    if (encoded_len > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    common::IoErr err =
            append_integer(static_cast<std::uint8_t>(first_byte_mask | huffman_mask), prefix_bits, encoded_len);
    if (err != common::IoErr::None || encoded_len == 0) {
        return err;
    }

    if (output_len_ < encoded_len) {
        err = ensure_output(encoded_len);
        if (err != common::IoErr::None) {
            return err;
        }
    }
    if (output_len_ >= encoded_len) {
        const std::size_t written = hpack_huffman_encode_exact(reinterpret_cast<const std::uint8_t *>(value.data()),
                                                               value.size(), output_dst_);
        if (written != encoded_len) {
            return common::IoErr::Invalid;
        }
        output_dst_ += written;
        output_len_ -= written;
        output_ops_.commit(output_ctx_, written);
        return common::IoErr::None;
    }

    HpackHuffmanEncodeState state;
    std::size_t consumed = 0;
    while (true) {
        if (output_len_ == 0) {
            err = ensure_output(1);
            if (err != common::IoErr::None) {
                return err;
            }
        }
        const auto result =
                hpack_huffman_encode_incremental(state, reinterpret_cast<const std::uint8_t *>(value.data()) + consumed,
                                                 value.size() - consumed, output_dst_, output_len_, true);
        consumed += result.consumed;
        output_dst_ += result.written;
        output_len_ -= result.written;
        output_ops_.commit(output_ctx_, result.written);
        if (result.code == HpackHuffmanCode::Ok) {
            return consumed == value.size() ? common::IoErr::None : common::IoErr::Invalid;
        }
        if (result.code != HpackHuffmanCode::OutputFull) {
            return common::IoErr::Invalid;
        }
    }
}

common::IoErr Http3QpackEncoder::append_integer(std::uint8_t first_byte_mask, std::uint8_t prefix_bits,
                                                std::uint64_t value) noexcept {
    std::array<std::uint8_t, kIntegerScratchCap> scratch{};
    const std::uint64_t prefix_max = (1ULL << prefix_bits) - 1ULL;
    std::size_t used = 0;
    if (value < prefix_max) {
        scratch[used++] = static_cast<std::uint8_t>(first_byte_mask | value);
        return append_bytes(scratch.data(), used);
    }

    scratch[used++] = static_cast<std::uint8_t>(first_byte_mask | prefix_max);
    value -= prefix_max;
    while (value >= 128U) {
        if (used >= scratch.size()) {
            return common::IoErr::Invalid;
        }
        scratch[used++] = static_cast<std::uint8_t>((value & 0x7fU) | 0x80U);
        value >>= 7U;
    }
    if (used >= scratch.size()) {
        return common::IoErr::Invalid;
    }
    scratch[used++] = static_cast<std::uint8_t>(value);
    return append_bytes(scratch.data(), used);
}

common::IoErr Http3QpackEncoder::append_bytes(const std::uint8_t *data, std::size_t len) noexcept {
    if (len == 0) {
        return common::IoErr::None;
    }
    if (data == nullptr) {
        return common::IoErr::Invalid;
    }

    std::size_t offset = 0;
    while (offset < len) {
        common::IoErr err = ensure_output(1);
        if (err != common::IoErr::None) {
            return err;
        }
        const std::size_t writable = std::min(output_len_, len - offset);
        std::memcpy(output_dst_, data + offset, writable);
        output_dst_ += writable;
        output_len_ -= writable;
        output_ops_.commit(output_ctx_, writable);
        offset += writable;
    }
    return common::IoErr::None;
}

common::IoErr Http3QpackEncoder::append_byte(std::uint8_t byte) noexcept { return append_bytes(&byte, 1); }

common::IoErr Http3QpackEncoder::ensure_output(std::size_t min_bytes) noexcept {
    if (min_bytes == 0 || output_len_ >= min_bytes) {
        return common::IoErr::None;
    }
    output_dst_ = nullptr;
    output_len_ = 0;
    common::IoErr err = output_ops_.acquire(output_ctx_, min_bytes, output_dst_, output_len_);
    if (err != common::IoErr::None) {
        return err;
    }
    return (output_dst_ != nullptr && output_len_ > 0) ? common::IoErr::None : common::IoErr::Invalid;
}

bool Http3QpackEncoder::should_huffman_encode(std::string_view value) const noexcept {
    return !value.empty() && value.size() >= options_.huffman_threshold;
}

} // namespace fiber::http
