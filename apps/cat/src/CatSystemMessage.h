#ifndef FIBER_CAT_SYSTEM_MESSAGE_H
#define FIBER_CAT_SYSTEM_MESSAGE_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include <common/mem/IoBuf.h>
#include <fiber/cat/CatClient.h>

#include "CatEncoder.h"
#include "CatSystemStats.h"

namespace fiber::cat::detail {

struct HeartbeatInfo {
    std::string_view app_key;
    std::string_view hostname;
    std::string_view ip;
    std::string_view client_version;
    std::uint64_t timestamp_millis = 0;
    std::uint64_t process_id = 0;
    std::uint64_t process_start_millis = 0;
    std::uint64_t uptime_millis = 0;
    std::size_t event_loop_count = 0;
    std::size_t collector_count = 0;
    std::uint64_t router_last_success_millis = 0;
    std::uint64_t sample_cutoff = 0;
    bool collector_connected = false;
    bool blocked = false;
    CatClientStats stats;
    const HeartbeatSystemStats *system_stats = nullptr;
};

[[nodiscard]] std::expected<mem::IoBuf, EncodeError>
encode_startup_nt1(const ClientEncodeContext &client, std::string_view message_id, std::string_view ip,
                   std::string_view client_version, CatEncoderType encoder = CatEncoderType::Nt1) noexcept;

[[nodiscard]] std::expected<mem::IoBuf, EncodeError>
encode_heartbeat_nt1(const ClientEncodeContext &client, std::string_view message_id, const HeartbeatInfo &info,
                     std::size_t max_fields, std::size_t max_data_bytes,
                     CatEncoderType encoder = CatEncoderType::Nt1) noexcept;

} // namespace fiber::cat::detail

#endif // FIBER_CAT_SYSTEM_MESSAGE_H
