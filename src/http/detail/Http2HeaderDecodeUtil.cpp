#include <fiber/http/detail/Http2HeaderDecodeUtil.h>

#include <cstring>

#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/Huffman.h>

namespace fiber::http::detail {

std::string_view copy_to_pool(mem::BufPool &pool, const std::uint8_t *data, std::size_t len) noexcept {
    if (len == 0) {
        return {};
    }
    auto *mem = static_cast<char *>(pool.alloc(len));
    if (!mem) {
        return {};
    }
    std::memcpy(mem, data, len);
    return {mem, len};
}

std::string_view copy_to_pool(mem::BufPool &pool, std::string_view value) noexcept {
    return copy_to_pool(pool, reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
}

common::IoErr materialize_name_raw(mem::BufPool &pool, const std::uint8_t *data, std::size_t len, std::string_view &out,
                                   std::uint64_t &name_hash) noexcept {
    out = copy_to_pool(pool, data, len);
    if (!out.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    name_hash = http_header_name_hash(out);
    return common::IoErr::None;
}

common::IoErr materialize_name_huffman(mem::BufPool &pool, const std::uint8_t *data, std::size_t len,
                                       std::string_view &out, std::uint64_t &name_hash) noexcept {
    out = {};
    bool ok = false;
    std::size_t decoded_len = hpack_huffman_decoded_length(data, len, &ok);
    if (!ok) {
        return common::IoErr::Invalid;
    }
    if (decoded_len != 0) {
        auto *mem = static_cast<char *>(pool.alloc(decoded_len));
        if (!mem) {
            return common::IoErr::NoMem;
        }
        HpackHuffmanDecodeState state;
        HpackHuffmanDecodeResult result =
                hpack_huffman_decode_exact(state, data, len, reinterpret_cast<std::uint8_t *>(mem), true);
        if (result.code != HpackHuffmanCode::Ok || result.written != decoded_len) {
            return common::IoErr::Invalid;
        }
        out = std::string_view(mem, decoded_len);
    }
    name_hash = http_header_name_hash(out);
    return common::IoErr::None;
}

common::IoErr materialize_value_raw(mem::BufPool &pool, const std::uint8_t *data, std::size_t len,
                                    std::string_view &out) noexcept {
    out = copy_to_pool(pool, data, len);
    if (!out.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    return common::IoErr::None;
}

common::IoErr materialize_value_huffman(mem::BufPool &pool, const std::uint8_t *data, std::size_t len,
                                        std::string_view &out) noexcept {
    out = {};
    bool ok = false;
    std::size_t decoded_len = hpack_huffman_decoded_length(data, len, &ok);
    if (!ok) {
        return common::IoErr::Invalid;
    }
    if (decoded_len == 0) {
        return common::IoErr::None;
    }
    auto *mem = static_cast<char *>(pool.alloc(decoded_len));
    if (!mem) {
        return common::IoErr::NoMem;
    }
    HpackHuffmanDecodeState state;
    HpackHuffmanDecodeResult result =
            hpack_huffman_decode_exact(state, data, len, reinterpret_cast<std::uint8_t *>(mem), true);
    if (result.code != HpackHuffmanCode::Ok || result.written != decoded_len) {
        return common::IoErr::Invalid;
    }
    out = std::string_view(mem, decoded_len);
    return common::IoErr::None;
}

} // namespace fiber::http::detail
