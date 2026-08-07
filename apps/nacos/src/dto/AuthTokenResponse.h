#ifndef FIBER_NACOS_DTO_AUTH_TOKEN_RESPONSE_H
#define FIBER_NACOS_DTO_AUTH_TOKEN_RESPONSE_H

#include <cstdint>
#include <string_view>

#include <fiber/common/json/JsonValue.h>

namespace fiber::nacos::dto::resp {

struct AuthTokenResponse {
    json::Nullable<std::string_view> access_token;
    std::int64_t token_ttl = 0;
};

} // namespace fiber::nacos::dto::resp

#endif // FIBER_NACOS_DTO_AUTH_TOKEN_RESPONSE_H
