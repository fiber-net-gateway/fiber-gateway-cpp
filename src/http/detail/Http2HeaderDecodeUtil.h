#ifndef FIBER_HTTP_DETAIL_HTTP2_HEADER_DECODE_UTIL_H
#define FIBER_HTTP_DETAIL_HTTP2_HEADER_DECODE_UTIL_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../common/IoError.h"
#include "../../common/mem/BufPool.h"

namespace fiber::http::detail {

[[nodiscard]] std::string_view copy_to_pool(mem::BufPool &pool, const std::uint8_t *data, std::size_t len) noexcept;
[[nodiscard]] std::string_view copy_to_pool(mem::BufPool &pool, std::string_view value) noexcept;

[[nodiscard]] common::IoErr materialize_name_raw(mem::BufPool &pool, const std::uint8_t *data, std::size_t len,
                                                 std::string_view &out, std::uint64_t &name_hash) noexcept;
[[nodiscard]] common::IoErr materialize_name_huffman(mem::BufPool &pool, const std::uint8_t *data, std::size_t len,
                                                     std::string_view &out, std::uint64_t &name_hash) noexcept;
[[nodiscard]] common::IoErr materialize_value_raw(mem::BufPool &pool, const std::uint8_t *data, std::size_t len,
                                                  std::string_view &out) noexcept;
[[nodiscard]] common::IoErr materialize_value_huffman(mem::BufPool &pool, const std::uint8_t *data, std::size_t len,
                                                      std::string_view &out) noexcept;

} // namespace fiber::http::detail

#endif // FIBER_HTTP_DETAIL_HTTP2_HEADER_DECODE_UTIL_H
