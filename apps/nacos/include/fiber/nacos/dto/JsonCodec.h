#ifndef FIBER_NACOS_DTO_JSON_CODEC_H
#define FIBER_NACOS_DTO_JSON_CODEC_H

#include <common/json/JsonEncode.h>
#include <common/json/JsonParse.h>

#include "AuthTokenResponse.h"
#include "Config.h"
#include "ConfigQueryRequest.h"
#include "Internal.h"
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

#define FIBER_NACOS_DECLARE_JSON_CODEC(Type)                                                                           \
    [[nodiscard]] json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, Type &out) noexcept;      \
    [[nodiscard]] json::Generator::Result encode_json(json::Generator &generator, const Type &value) noexcept

FIBER_NACOS_DECLARE_JSON_CODEC(RequestBase);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ServerCheckRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ConnectionSetupRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::HealthCheckRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::SetupAckRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ClientDetectionRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ConnectResetRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ServerCheckResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::HealthCheckResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ClientDetectionResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ConnectResetResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ErrorResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ConfigPublishRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ConfigRemoveRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ConfigBatchListenRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(req::ConfigChangeNotifyRequest);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ConfigQueryResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ConfigPublishResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ConfigRemoveResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ConfigChangeBatchListenResponse);
FIBER_NACOS_DECLARE_JSON_CODEC(resp::ConfigChangeNotifyResponse);

#undef FIBER_NACOS_DECLARE_JSON_CODEC

} // namespace fiber::nacos::dto

#endif // FIBER_NACOS_DTO_JSON_CODEC_H
