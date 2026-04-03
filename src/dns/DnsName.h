#ifndef FIBER_DNS_DNS_NAME_H
#define FIBER_DNS_DNS_NAME_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../common/IoError.h"

namespace fiber::dns {

struct DecodedName {
    std::string_view name{};
    std::size_t next_offset = 0;
};

[[nodiscard]] common::IoResult<std::size_t> encode_name(std::string_view name,
                                                        std::uint8_t *dst,
                                                        std::size_t cap) noexcept;

[[nodiscard]] common::IoErr normalize_name(std::string_view input,
                                           char *dst,
                                           std::size_t cap,
                                           std::string_view &out) noexcept;

[[nodiscard]] common::IoResult<DecodedName> decode_name(const std::uint8_t *packet,
                                                        std::size_t packet_len,
                                                        std::size_t offset,
                                                        char *storage,
                                                        std::size_t storage_cap) noexcept;

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_NAME_H
