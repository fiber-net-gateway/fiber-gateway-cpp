#include <fiber/nacos/dto/JsonCodec.h>

namespace fiber::nacos::dto {
namespace {

struct AuthTokenResponsePresence {
    bool access_token = false;
    bool username = false;
};

json::ObjectFieldStatus parse_auth_token_response_field(AuthTokenResponsePresence &presence, std::string_view field,
                                                        json::JsonParser &parser, mem::BufPool &pool,
                                                        resp::AuthTokenResponse &out) noexcept {
    if (field == "accessToken") {
        presence.access_token = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.access_token));
    }
    if (field == "tokenTtl") {
        return json::to_object_field_status(json::parse_integral<std::int64_t>(parser, pool, out.token_ttl));
    }
    if (field == "globalAdmin") {
        return json::to_object_field_status(json::parse_bool(parser, pool, out.global_admin));
    }
    if (field == "username") {
        presence.username = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.username));
    }
    return json::ObjectFieldStatus::Unknown;
}

} // namespace

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, resp::AuthTokenResponse &out) noexcept {
    AuthTokenResponsePresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    resp::AuthTokenResponse &value) noexcept {
        return parse_auth_token_response_field(presence, field, field_parser, field_pool, value);
    };

    const json::ParseStatus status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    if (!presence.access_token) {
        out.access_token.set_absent();
    }
    if (!presence.username) {
        out.username.set_absent();
    }
    return json::ParseStatus::Done;
}

} // namespace fiber::nacos::dto
