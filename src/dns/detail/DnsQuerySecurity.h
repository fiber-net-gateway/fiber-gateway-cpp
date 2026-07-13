#ifndef FIBER_DNS_DETAIL_DNS_QUERY_SECURITY_H
#define FIBER_DNS_DETAIL_DNS_QUERY_SECURITY_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/IoError.h"

namespace fiber::dns::detail {

inline constexpr std::size_t kDnsQueryIdCount = 1U << 16U;
inline constexpr std::size_t kDns0x20RandomBytes = 32;

[[nodiscard]] common::IoResult<std::uint16_t> select_query_id(const std::uint16_t *id_to_slot,
                                                              std::uint16_t invalid_mapping, std::uint16_t start,
                                                              std::uint16_t stride) noexcept;

[[nodiscard]] common::IoErr
apply_query_name_0x20(std::uint8_t *packet, std::size_t packet_len,
                      std::span<const std::uint8_t, kDns0x20RandomBytes> random_bits) noexcept;

[[nodiscard]] bool response_matches_query(const std::uint8_t *request, std::size_t request_len,
                                          const std::uint8_t *response, std::size_t response_len,
                                          bool exact_name_case) noexcept;

} // namespace fiber::dns::detail

#endif // FIBER_DNS_DETAIL_DNS_QUERY_SECURITY_H
