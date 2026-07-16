#ifndef FIBER_NACOS_DTO_JSON_CODEC_H
#define FIBER_NACOS_DTO_JSON_CODEC_H

#include <common/json/JsonEncode.h>
#include <common/json/JsonParse.h>

#include "AuthTokenResponse.h"
#include "ConfigQueryRequest.h"
#include "NotifySubscriberResponse.h"

namespace fiber::nacos::dto {

[[nodiscard]] json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool,
                                           req::ConfigQueryRequest &out) noexcept;
[[nodiscard]] json::Generator::Result encode_json(json::Generator &generator,
                                                  const req::ConfigQueryRequest &value) noexcept;

[[nodiscard]] json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool,
                                           resp::NotifySubscriberResponse &out) noexcept;
[[nodiscard]] json::Generator::Result encode_json(json::Generator &generator,
                                                  const resp::NotifySubscriberResponse &value) noexcept;

[[nodiscard]] json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool,
                                           resp::AuthTokenResponse &out) noexcept;

} // namespace fiber::nacos::dto

#endif // FIBER_NACOS_DTO_JSON_CODEC_H
