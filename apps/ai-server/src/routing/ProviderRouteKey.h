#ifndef FIBER_AI_SERVER_PROVIDER_ROUTE_KEY_H
#define FIBER_AI_SERVER_PROVIDER_ROUTE_KEY_H

#include "../config/LlmConfigSnapshot.h"
#include "../protocol/LlmBody.h"

#include <expected>
#include <string_view>

#include <fiber/common/mem/BufPool.h>

namespace fiber::ai_server {

enum class ProviderRouteKeyError : std::uint8_t {
    OutOfMemory,
};

[[nodiscard]] std::expected<std::string_view, ProviderRouteKeyError>
build_provider_route_key(LlmWireProtocol protocol, const LlmRoutingData &routing, const LoadBalanceConfig &config,
                         mem::BufPool &pool) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_ROUTE_KEY_H
