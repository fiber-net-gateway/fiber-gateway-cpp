#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
namespace {

template<typename T>
json::Generator::Result encode_empty_request(json::Generator &generator, const T &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module",
                                  [&]() noexcept { return detail::encode_text(generator, T::kModule); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

template<typename T>
json::Generator::Result encode_empty_response(json::Generator &generator, const T &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_response_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "success",
                                  [&]() noexcept { return generator.bool_value(value.success()); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

} // namespace

json::Generator::Result encode_json(json::Generator &generator, const RequestBase &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

#define FIBER_NACOS_DEFINE_EMPTY_REQUEST(Type)                                                                         \
    json::Generator::Result encode_json(json::Generator &generator, const Type &value) noexcept {                      \
        return encode_empty_request(generator, value);                                                                 \
    }

FIBER_NACOS_DEFINE_EMPTY_REQUEST(req::ServerCheckRequest)
FIBER_NACOS_DEFINE_EMPTY_REQUEST(req::HealthCheckRequest)
FIBER_NACOS_DEFINE_EMPTY_REQUEST(req::ClientDetectionRequest)

#undef FIBER_NACOS_DEFINE_EMPTY_REQUEST

json::Generator::Result encode_json(json::Generator &generator, const req::ConnectionSetupRequest &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "clientVersion", value.client_version);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "tenant", value.tenant);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_object_field(generator, "labels", value.labels, [&](std::string_view text) {
        return detail::encode_text(generator, text);
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_object_field(generator, "abilityTable", value.ability_table,
                                                  [&](bool enabled) { return generator.bool_value(enabled); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module", [&]() noexcept {
        return detail::encode_text(generator, req::ConnectionSetupRequest::kModule);
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

json::Generator::Result encode_json(json::Generator &generator, const req::SetupAckRequest &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_object_field(generator, "abilityTable", value.ability_table,
                                                  [&](bool enabled) { return generator.bool_value(enabled); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module", [&]() noexcept {
        return detail::encode_text(generator, req::SetupAckRequest::kModule);
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

json::Generator::Result encode_json(json::Generator &generator, const req::ConnectResetRequest &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    for (const auto &[key, field]:
         {std::pair{"serverIp", &value.server_ip}, std::pair{"serverPort", &value.server_port},
          std::pair{"connectionId", &value.connection_id}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != json::Generator::Result::OK) {
            return result;
        }
    }
    result = detail::encode_field(generator, "module", [&]() noexcept {
        return detail::encode_text(generator, req::ConnectResetRequest::kModule);
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

json::Generator::Result encode_json(json::Generator &generator, const resp::ServerCheckResponse &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_response_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "connectionId", value.connection_id);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "supportAbilityNegotiation",
                                  [&]() noexcept { return generator.bool_value(value.support_ability_negotiation); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "success",
                                  [&]() noexcept { return generator.bool_value(value.success()); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

#define FIBER_NACOS_DEFINE_EMPTY_RESPONSE(Type)                                                                        \
    json::Generator::Result encode_json(json::Generator &generator, const Type &value) noexcept {                      \
        return encode_empty_response(generator, value);                                                                \
    }

FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::HealthCheckResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ClientDetectionResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConnectResetResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ErrorResponse)

#undef FIBER_NACOS_DEFINE_EMPTY_RESPONSE

} // namespace fiber::nacos::dto
