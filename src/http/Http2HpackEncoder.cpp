#include <fiber/http/Http2HpackEncoder.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <fiber/http/Http2HpackStaticTable.h>
#include "http/Huffman.h"

namespace fiber::http {

namespace {

constexpr std::size_t kIntegerScratchCap = 8;
constexpr std::uint8_t kTableSizeZero = 0x20;
constexpr std::string_view kProtocolName = ":protocol";

namespace pseudo_index {
constexpr std::uint32_t kAuthority = 1;
constexpr std::uint32_t kMethod = 2;
constexpr std::uint32_t kMethodGet = 2;
constexpr std::uint32_t kMethodPost = 3;
constexpr std::uint32_t kPath = 4;
constexpr std::uint32_t kPathRoot = 4;
constexpr std::uint32_t kPathIndexHtml = 5;
constexpr std::uint32_t kScheme = 6;
constexpr std::uint32_t kSchemeHttp = 6;
constexpr std::uint32_t kSchemeHttps = 7;
constexpr std::uint32_t kStatus = 8;
constexpr std::uint32_t kStatus200 = 8;
constexpr std::uint32_t kStatus204 = 9;
constexpr std::uint32_t kStatus206 = 10;
constexpr std::uint32_t kStatus304 = 11;
constexpr std::uint32_t kStatus400 = 12;
constexpr std::uint32_t kStatus404 = 13;
constexpr std::uint32_t kStatus500 = 14;
} // namespace pseudo_index

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

Http2HpackEncoder::Http2HpackEncoder(Options options) noexcept : options_(options) {}

void Http2HpackEncoder::reset_block() noexcept {
    output_ctx_ = nullptr;
    output_ops_ = nullptr;
    output_dst_ = nullptr;
    output_len_ = 0;
    block_open_ = false;
}

common::IoErr Http2HpackEncoder::begin_block(void *output_ctx, const OutputOps *output_ops) noexcept {
    if (block_open_) {
        return common::IoErr::Invalid;
    }
    if (output_ctx == nullptr || output_ops == nullptr || output_ops->acquire == nullptr ||
        output_ops->commit == nullptr) {
        return common::IoErr::Invalid;
    }

    output_ctx_ = output_ctx;
    output_ops_ = output_ops;
    output_dst_ = nullptr;
    output_len_ = 0;
    block_open_ = true;

    common::IoErr err = append_byte(kTableSizeZero);
    if (err != common::IoErr::None) {
        reset_block();
        return err;
    }
    return common::IoErr::None;
}

common::IoErr Http2HpackEncoder::encode_status(int status_code) noexcept {
    std::uint32_t exact_index = 0;
    switch (status_code) {
        case 200:
            exact_index = pseudo_index::kStatus200;
            break;
        case 204:
            exact_index = pseudo_index::kStatus204;
            break;
        case 206:
            exact_index = pseudo_index::kStatus206;
            break;
        case 304:
            exact_index = pseudo_index::kStatus304;
            break;
        case 400:
            exact_index = pseudo_index::kStatus400;
            break;
        case 404:
            exact_index = pseudo_index::kStatus404;
            break;
        case 500:
            exact_index = pseudo_index::kStatus500;
            break;
        default:
            break;
    }
    if (exact_index != 0) {
        return encode_pseudo(exact_index, pseudo_index::kStatus, {}, {});
    }

    std::array<char, 3> scratch{};
    const std::string_view value = format_status_value(status_code, scratch);
    if (value.empty()) {
        return common::IoErr::Invalid;
    }
    return encode_pseudo(0, pseudo_index::kStatus, {}, value);
}

common::IoErr Http2HpackEncoder::encode_method(HttpMethod method) noexcept {
    if (method == HttpMethod::Get) {
        return encode_pseudo(pseudo_index::kMethodGet, pseudo_index::kMethod, {}, {});
    }
    if (method == HttpMethod::Post) {
        return encode_pseudo(pseudo_index::kMethodPost, pseudo_index::kMethod, {}, {});
    }
    const std::string_view value = common_method_value(method);
    if (value.empty()) {
        return common::IoErr::Invalid;
    }
    return encode_pseudo(0, pseudo_index::kMethod, {}, value);
}

common::IoErr Http2HpackEncoder::encode_scheme(std::string_view scheme) noexcept {
    if (scheme == "http") {
        return encode_pseudo(pseudo_index::kSchemeHttp, pseudo_index::kScheme, {}, {});
    }
    if (scheme == "https") {
        return encode_pseudo(pseudo_index::kSchemeHttps, pseudo_index::kScheme, {}, {});
    }
    return encode_pseudo(0, pseudo_index::kScheme, {}, scheme);
}

common::IoErr Http2HpackEncoder::encode_authority(std::string_view authority) noexcept {
    if (authority.empty()) {
        return encode_pseudo(pseudo_index::kAuthority, pseudo_index::kAuthority, {}, {});
    }
    return encode_pseudo(0, pseudo_index::kAuthority, {}, authority);
}

common::IoErr Http2HpackEncoder::encode_path(std::string_view path) noexcept {
    if (path == "/") {
        return encode_pseudo(pseudo_index::kPathRoot, pseudo_index::kPath, {}, {});
    }
    if (path == "/index.html") {
        return encode_pseudo(pseudo_index::kPathIndexHtml, pseudo_index::kPath, {}, {});
    }
    return encode_pseudo(0, pseudo_index::kPath, {}, path);
}

common::IoErr Http2HpackEncoder::encode_protocol(std::string_view protocol) noexcept {
    return encode_pseudo(0, 0, kProtocolName, protocol);
}

common::IoErr Http2HpackEncoder::encode_field(std::string_view name, std::uint64_t name_hash,
                                              std::string_view value) noexcept {
    if (!block_open_) {
        return common::IoErr::Invalid;
    }
    if (name.size() > options_.max_string_size || value.size() > options_.max_string_size) {
        return common::IoErr::Invalid;
    }

    const Http2HpackStaticTable::FindResult result = Http2HpackStaticTable::find(name, name_hash, value);
    if (result.exact_index != 0) {
        return append_indexed(result.exact_index);
    }

    if (result.name_index != 0) {
        return append_literal(result.name_index, {}, value);
    }
    return append_literal(0, name, value);
}

common::IoErr Http2HpackEncoder::finish_block() noexcept {
    if (!block_open_) {
        return common::IoErr::Invalid;
    }
    reset_block();
    return common::IoErr::None;
}

void Http2HpackEncoder::cancel_block() noexcept {
    if (!block_open_) {
        return;
    }
    reset_block();
}

common::IoErr Http2HpackEncoder::encode_pseudo(std::uint32_t exact_index, std::uint32_t name_index,
                                               std::string_view name, std::string_view value) noexcept {
    if (!block_open_ || value.size() > options_.max_string_size ||
        (name_index == 0 && (name.empty() || name.size() > options_.max_string_size))) {
        return common::IoErr::Invalid;
    }
    if (exact_index != 0) {
        return append_byte(static_cast<std::uint8_t>(0x80U | exact_index));
    }
    return append_literal(name_index, name, value);
}

common::IoErr Http2HpackEncoder::append_indexed(std::uint32_t index) noexcept {
    return append_integer(0x80U, 7, index);
}

common::IoErr Http2HpackEncoder::append_literal(std::uint32_t name_index, std::string_view name,
                                                std::string_view value) noexcept {
    const bool new_name = name_index == 0;
    common::IoErr err = append_integer(0x00U, 4, name_index);
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

    // RFC 7541 §6.2: Huffman SHOULD only be used when it actually shortens the
    // string. should_huffman_encode() is a cheap O(1) threshold gate that skips
    // the O(n) length scan for short strings; when it passes we still compare the
    // real encoded length against the raw size before committing to Huffman.
    std::size_t encoded_len = 0;
    bool use_huffman = should_huffman_encode(value);
    if (use_huffman) {
        encoded_len = hpack_huffman_encoded_length(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
        use_huffman = encoded_len < value.size();
    }

    if (!use_huffman) {
        common::IoErr err = append_integer(0x00U, 7, static_cast<std::uint32_t>(value.size()));
        if (err != common::IoErr::None) {
            return err;
        }
        return append_bytes(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    }

    common::IoErr err = append_integer(0x80U, 7, static_cast<std::uint32_t>(encoded_len));
    if (err != common::IoErr::None) {
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
        output_ops_->commit(output_ctx_, written);
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
        output_ops_->commit(output_ctx_, result.written);
        if (result.code == HpackHuffmanCode::Ok) {
            return consumed == value.size() ? common::IoErr::None : common::IoErr::Invalid;
        }
        if (result.code != HpackHuffmanCode::OutputFull) {
            return common::IoErr::Invalid;
        }
    }
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
    if (len == 0 || data == nullptr) {
        return common::IoErr::None;
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
        output_ops_->commit(output_ctx_, writable);
        offset += writable;
    }
    return common::IoErr::None;
}

common::IoErr Http2HpackEncoder::append_byte(std::uint8_t byte) noexcept { return append_bytes(&byte, 1); }

common::IoErr Http2HpackEncoder::ensure_output(std::size_t min_bytes) noexcept {
    if (output_len_ >= min_bytes) {
        return common::IoErr::None;
    }
    if (output_ops_ == nullptr || output_ctx_ == nullptr) {
        return common::IoErr::Invalid;
    }
    output_dst_ = nullptr;
    output_len_ = 0;
    common::IoErr err = output_ops_->acquire(output_ctx_, min_bytes, output_dst_, output_len_);
    if (err != common::IoErr::None) {
        return err;
    }
    return (output_dst_ != nullptr && output_len_ > 0) ? common::IoErr::None : common::IoErr::Invalid;
}

bool Http2HpackEncoder::should_huffman_encode(std::string_view value) const noexcept {
    // Cheap O(1) threshold gate only; the encoded-vs-raw benefit check lives in
    // append_string, which falls back to raw when Huffman would not shorten.
    return value.size() >= options_.huffman_threshold;
}

} // namespace fiber::http
