#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
namespace {

json::ParseStatus parse_string_object(json::JsonParser &parser, mem::BufPool &pool,
                                      json::JsonObject<std::string_view> &out) noexcept {
    return json::parse_object<json::parse_text>(parser, pool, out);
}

json::ParseStatus parse_bool_object(json::JsonParser &parser, mem::BufPool &pool,
                                    json::JsonObject<bool> &out) noexcept {
    return json::parse_object<json::parse_bool>(parser, pool, out);
}

template<typename T>
json::ParseStatus parse_empty_request(json::JsonParser &parser, mem::BufPool &pool, T &out) noexcept {
    detail::RequestPresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    T &value) noexcept {
        auto status = detail::parse_request_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "module") {
            return detail::parse_module(T::kModule, field_parser, field_pool);
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status == json::ParseStatus::Done) {
        detail::finish_presence(presence, out);
    }
    return status;
}

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
json::ParseStatus parse_empty_response(json::JsonParser &parser, mem::BufPool &pool, T &out) noexcept {
    detail::ResponsePresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    T &value) noexcept {
        return detail::parse_response_base_field(presence, field, field_parser, field_pool, value);
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status == json::ParseStatus::Done) {
        detail::finish_presence(presence, out);
    }
    return status;
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

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, RequestBase &out) noexcept {
    detail::RequestPresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    RequestBase &value) noexcept {
        return detail::parse_request_base_field(presence, field, field_parser, field_pool, value);
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status == json::ParseStatus::Done) {
        detail::finish_presence(presence, out);
    }
    return status;
}

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
    json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, Type &out) noexcept {                   \
        return parse_empty_request(parser, pool, out);                                                                 \
    }                                                                                                                  \
    json::Generator::Result encode_json(json::Generator &generator, const Type &value) noexcept {                      \
        return encode_empty_request(generator, value);                                                                 \
    }

FIBER_NACOS_DEFINE_EMPTY_REQUEST(req::ServerCheckRequest)
FIBER_NACOS_DEFINE_EMPTY_REQUEST(req::HealthCheckRequest)
FIBER_NACOS_DEFINE_EMPTY_REQUEST(req::ClientDetectionRequest)

#undef FIBER_NACOS_DEFINE_EMPTY_REQUEST

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, req::ConnectionSetupRequest &out) noexcept {
    struct Presence : detail::RequestPresence {
        bool client_version = false;
        bool tenant = false;
        bool labels = false;
        bool ability_table = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::ConnectionSetupRequest &value) noexcept {
        auto status = detail::parse_request_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "clientVersion") {
            presence.client_version = true;
            return json::to_object_field_status(
                    json::parse_nullable<json::parse_text>(field_parser, field_pool, value.client_version));
        }
        if (field == "tenant") {
            presence.tenant = true;
            return json::to_object_field_status(
                    json::parse_nullable<json::parse_text>(field_parser, field_pool, value.tenant));
        }
        if (field == "labels") {
            presence.labels = true;
            return json::to_object_field_status(
                    json::parse_nullable<parse_string_object>(field_parser, field_pool, value.labels));
        }
        if (field == "abilityTable") {
            presence.ability_table = true;
            return json::to_object_field_status(
                    json::parse_nullable<parse_bool_object>(field_parser, field_pool, value.ability_table));
        }
        if (field == "module") {
            return detail::parse_module(req::ConnectionSetupRequest::kModule, field_parser, field_pool);
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    detail::finish_presence(presence, out);
    if (!presence.client_version) {
        out.client_version.set_absent();
    }
    if (!presence.tenant) {
        out.tenant.set_absent();
    }
    if (!presence.labels) {
        out.labels.set_absent();
    }
    if (!presence.ability_table) {
        out.ability_table.set_absent();
    }
    return json::ParseStatus::Done;
}

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

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, req::SetupAckRequest &out) noexcept {
    struct Presence : detail::RequestPresence {
        bool ability_table = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::SetupAckRequest &value) noexcept {
        auto status = detail::parse_request_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "abilityTable") {
            presence.ability_table = true;
            return json::to_object_field_status(
                    json::parse_nullable<parse_bool_object>(field_parser, field_pool, value.ability_table));
        }
        if (field == "module") {
            return detail::parse_module(req::SetupAckRequest::kModule, field_parser, field_pool);
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    detail::finish_presence(presence, out);
    if (!presence.ability_table) {
        out.ability_table.set_absent();
    }
    return json::ParseStatus::Done;
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

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, req::ConnectResetRequest &out) noexcept {
    struct Presence : detail::RequestPresence {
        bool server_ip = false;
        bool server_port = false;
        bool connection_id = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::ConnectResetRequest &value) noexcept {
        auto status = detail::parse_request_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        json::Nullable<std::string_view> *target = nullptr;
        bool *present = nullptr;
        if (field == "serverIp") {
            target = &value.server_ip;
            present = &presence.server_ip;
        } else if (field == "serverPort") {
            target = &value.server_port;
            present = &presence.server_port;
        } else if (field == "connectionId") {
            target = &value.connection_id;
            present = &presence.connection_id;
        } else if (field == "module") {
            return detail::parse_module(req::ConnectResetRequest::kModule, field_parser, field_pool);
        } else {
            return json::ObjectFieldStatus::Unknown;
        }
        *present = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(field_parser, field_pool, *target));
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    detail::finish_presence(presence, out);
    if (!presence.server_ip) {
        out.server_ip.set_absent();
    }
    if (!presence.server_port) {
        out.server_port.set_absent();
    }
    if (!presence.connection_id) {
        out.connection_id.set_absent();
    }
    return json::ParseStatus::Done;
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

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, resp::ServerCheckResponse &out) noexcept {
    struct Presence : detail::ResponsePresence {
        bool connection_id = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    resp::ServerCheckResponse &value) noexcept {
        auto status = detail::parse_response_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "connectionId") {
            presence.connection_id = true;
            return json::to_object_field_status(
                    json::parse_nullable<json::parse_text>(field_parser, field_pool, value.connection_id));
        }
        if (field == "supportAbilityNegotiation") {
            return json::to_object_field_status(
                    json::parse_bool(field_parser, field_pool, value.support_ability_negotiation));
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    detail::finish_presence(presence, out);
    if (!presence.connection_id) {
        out.connection_id.set_absent();
    }
    return json::ParseStatus::Done;
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
    json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, Type &out) noexcept {                   \
        return parse_empty_response(parser, pool, out);                                                                \
    }                                                                                                                  \
    json::Generator::Result encode_json(json::Generator &generator, const Type &value) noexcept {                      \
        return encode_empty_response(generator, value);                                                                \
    }

FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::HealthCheckResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ClientDetectionResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConnectResetResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ErrorResponse)

#undef FIBER_NACOS_DEFINE_EMPTY_RESPONSE

} // namespace fiber::nacos::dto
