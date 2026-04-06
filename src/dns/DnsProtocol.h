#ifndef FIBER_DNS_DNS_PROTOCOL_H
#define FIBER_DNS_DNS_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::dns {

enum class RecordType : std::uint16_t {
    A = 1,
    NS = 2,
    CNAME = 5,
    SOA = 6,
    PTR = 12,
    MX = 15,
    TXT = 16,
    AAAA = 28,
    SRV = 33,
    OPT = 41,
    HTTPS = 65,
};

enum class RecordClass : std::uint16_t {
    IN = 1,
};

enum class RCode : std::uint8_t {
    NoError = 0,
    FormatError = 1,
    ServerFailure = 2,
    NxDomain = 3,
    NotImplemented = 4,
    Refused = 5,
};

struct Header {
    std::uint16_t id = 0;
    std::uint16_t flags = 0;
    std::uint16_t question_count = 0;
    std::uint16_t answer_count = 0;
    std::uint16_t authority_count = 0;
    std::uint16_t additional_count = 0;

    [[nodiscard]] bool is_response() const noexcept { return (flags & 0x8000U) != 0; }
    [[nodiscard]] std::uint8_t opcode() const noexcept { return static_cast<std::uint8_t>((flags >> 11U) & 0x0fU); }
    [[nodiscard]] bool authoritative_answer() const noexcept { return (flags & 0x0400U) != 0; }
    [[nodiscard]] bool truncated() const noexcept { return (flags & 0x0200U) != 0; }
    [[nodiscard]] bool recursion_desired() const noexcept { return (flags & 0x0100U) != 0; }
    [[nodiscard]] bool recursion_available() const noexcept { return (flags & 0x0080U) != 0; }
    [[nodiscard]] bool authentic_data() const noexcept { return (flags & 0x0020U) != 0; }
    [[nodiscard]] bool checking_disabled() const noexcept { return (flags & 0x0010U) != 0; }
    [[nodiscard]] RCode rcode() const noexcept { return static_cast<RCode>(flags & 0x000fU); }
};

struct QuestionSpec {
    std::string_view name{};
    std::uint16_t type = static_cast<std::uint16_t>(RecordType::A);
    std::uint16_t dns_class = static_cast<std::uint16_t>(RecordClass::IN);
};

struct QueryOptions {
    std::uint16_t id = 0;
    bool recursion_desired = true;
    bool checking_disabled = false;
    bool use_edns = true;
    std::uint16_t max_udp_payload_size = 1232;
    std::uint8_t edns_version = 0;
    bool dnssec_ok = false;
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_PROTOCOL_H
