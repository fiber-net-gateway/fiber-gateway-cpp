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

// Pseudo-header static-table indices (RFC 9204). The QPACK static table is fixed
// by spec; these mirror the order in Http3QpackStaticTable.cpp. Used to resolve
// pseudo-headers in O(1) without scanning the table.
namespace pseudo_index {
constexpr std::uint32_t kAuthority = 0; // :authority ""
constexpr std::uint32_t kPath = 1; // :path "/"
constexpr std::uint32_t kMethodFirst = 15; // first :method entry (CONNECT)
constexpr std::uint32_t kSchemeFirst = 22; // first :scheme entry (http)
constexpr std::uint32_t kStatusFirst = 24; // first :status entry (103)
} // namespace pseudo_index

// Each resolver returns the FindResult the general find() would produce for the
// given pseudo-header value, but in O(1). Exact (name,value) matches map to the
// unique static index; anything else falls back to the first same-name index
// (NameMatch), exactly as the linear find() would.

[[nodiscard]] Http3QpackStaticTable::FindResult resolve_status(int status_code) noexcept {
    using K = Http3QpackStaticTable::FindKind;
    std::uint32_t idx;
    switch (status_code) {
        case 103:
            idx = 24;
            break;
        case 200:
            idx = 25;
            break;
        case 304:
            idx = 26;
            break;
        case 404:
            idx = 27;
            break;
        case 503:
            idx = 28;
            break;
        case 100:
            idx = 63;
            break;
        case 204:
            idx = 64;
            break;
        case 206:
            idx = 65;
            break;
        case 302:
            idx = 66;
            break;
        case 400:
            idx = 67;
            break;
        case 403:
            idx = 68;
            break;
        case 421:
            idx = 69;
            break;
        case 425:
            idx = 70;
            break;
        case 500:
            idx = 71;
            break;
        default:
            return {.kind = K::NameMatch, .index = pseudo_index::kStatusFirst};
    }
    return {.kind = K::EntryMatch, .index = idx};
}

[[nodiscard]] Http3QpackStaticTable::FindResult resolve_method(HttpMethod method) noexcept {
    using K = Http3QpackStaticTable::FindKind;
    std::uint32_t idx;
    switch (method) {
        case HttpMethod::Connect:
            idx = 15;
            break;
        case HttpMethod::Delete:
            idx = 16;
            break;
        case HttpMethod::Get:
            idx = 17;
            break;
        case HttpMethod::Head:
            idx = 18;
            break;
        case HttpMethod::Options:
            idx = 19;
            break;
        case HttpMethod::Post:
            idx = 20;
            break;
        case HttpMethod::Put:
            idx = 21;
            break;
        default:
            return {.kind = K::NameMatch, .index = pseudo_index::kMethodFirst};
    }
    return {.kind = K::EntryMatch, .index = idx};
}

[[nodiscard]] Http3QpackStaticTable::FindResult resolve_scheme(std::string_view scheme) noexcept {
    using K = Http3QpackStaticTable::FindKind;
    if (scheme == "https") {
        return {.kind = K::EntryMatch, .index = 23};
    }
    if (scheme == "http") {
        return {.kind = K::EntryMatch, .index = 22};
    }
    return {.kind = K::NameMatch, .index = pseudo_index::kSchemeFirst};
}

[[nodiscard]] Http3QpackStaticTable::FindResult resolve_path(std::string_view path) noexcept {
    using K = Http3QpackStaticTable::FindKind;
    if (path == "/") {
        return {.kind = K::EntryMatch, .index = pseudo_index::kPath};
    }
    return {.kind = K::NameMatch, .index = pseudo_index::kPath};
}

[[nodiscard]] Http3QpackStaticTable::FindResult resolve_authority(std::string_view authority) noexcept {
    using K = Http3QpackStaticTable::FindKind;
    if (authority.empty()) {
        return {.kind = K::EntryMatch, .index = pseudo_index::kAuthority};
    }
    return {.kind = K::NameMatch, .index = pseudo_index::kAuthority};
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
    return dispatch(kStatusName, value, resolve_status(status_code));
}

common::IoErr Http3QpackEncoder::encode_method(HttpMethod method) noexcept {
    const std::string_view value = common_method_value(method);
    if (value.empty()) {
        return common::IoErr::Invalid;
    }
    return dispatch(kMethodName, value, resolve_method(method));
}

common::IoErr Http3QpackEncoder::encode_scheme(std::string_view scheme) noexcept {
    return dispatch(kSchemeName, scheme, resolve_scheme(scheme));
}

common::IoErr Http3QpackEncoder::encode_authority(std::string_view authority) noexcept {
    return dispatch(kAuthorityName, authority, resolve_authority(authority));
}

common::IoErr Http3QpackEncoder::encode_path(std::string_view path) noexcept {
    return dispatch(kPathName, path, resolve_path(path));
}

common::IoErr Http3QpackEncoder::encode_field(std::string_view name, std::uint64_t name_hash,
                                              std::string_view value) noexcept {
    const std::uint64_t effective_name_hash = name_hash != 0 ? name_hash : http_header_name_hash(name);
    return dispatch(name, value, Http3QpackStaticTable::find(name, effective_name_hash, value));
}

common::IoErr Http3QpackEncoder::dispatch(std::string_view name, std::string_view value,
                                          const Http3QpackStaticTable::FindResult &result) noexcept {
    FIBER_ASSERT(!finished_);
    if (name.empty() || name.size() > options_.max_string_size || value.size() > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = ensure_prefix();
    if (err != common::IoErr::None) {
        return err;
    }

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

    // Huffman only helps when it shortens the string; the H bit (RFC 9204 §4.5)
    // is set only then. should_huffman_encode() is a cheap O(1) threshold gate
    // that skips the O(n) length scan for short strings; when it passes we still
    // compare the real encoded length against the raw size before committing.
    std::size_t encoded_len = 0;
    bool use_huffman = should_huffman_encode(value);
    if (use_huffman) {
        encoded_len = hpack_huffman_encoded_length(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
        use_huffman = encoded_len < value.size();
    }

    if (!use_huffman) {
        common::IoErr err = append_integer(first_byte_mask, prefix_bits, value.size());
        if (err != common::IoErr::None) {
            return err;
        }
        return append_bytes(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
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
    // Cheap O(1) threshold gate only; the encoded-vs-raw benefit check lives in
    // append_prefixed_string, which falls back to raw when Huffman would not shorten.
    return !value.empty() && value.size() >= options_.huffman_threshold;
}

} // namespace fiber::http
