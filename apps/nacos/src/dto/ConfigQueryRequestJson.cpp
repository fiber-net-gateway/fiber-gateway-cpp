#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
namespace {

struct ConfigQueryPresence {
    bool request_id = false;
    bool data_id = false;
    bool group = false;
    bool tenant = false;
    bool tag = false;
};

json::ObjectFieldStatus parse_config_query_field(ConfigQueryPresence &presence, std::string_view field,
                                                 json::JsonParser &parser, mem::BufPool &pool,
                                                 req::ConfigQueryRequest &out) noexcept {
    if (field == "requestId") {
        presence.request_id = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.request_id));
    }
    if (field == "dataId") {
        presence.data_id = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.data_id));
    }
    if (field == "group") {
        presence.group = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.group));
    }
    if (field == "tenant") {
        presence.tenant = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.tenant));
    }
    if (field == "tag") {
        presence.tag = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.tag));
    }
    if (field == "notify") {
        return json::to_object_field_status(json::parse_bool(parser, pool, out.notify));
    }
    if (field == "module") {
        std::string_view module;
        if (json::parse_text(parser, pool, module) != json::ParseStatus::Done) {
            return json::ObjectFieldStatus::Error;
        }
        if (module != req::ConfigQueryRequest::kModule) {
            (void) parser.fail("unexpected Nacos request module");
            return json::ObjectFieldStatus::Error;
        }
        return json::ObjectFieldStatus::Parsed;
    }
    return json::ObjectFieldStatus::Unknown;
}

} // namespace

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, req::ConfigQueryRequest &out) noexcept {
    ConfigQueryPresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::ConfigQueryRequest &value) noexcept {
        return parse_config_query_field(presence, field, field_parser, field_pool, value);
    };

    const json::ParseStatus status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    if (!presence.request_id) {
        out.request_id.set_absent();
    }
    if (!presence.data_id) {
        out.data_id.set_absent();
    }
    if (!presence.group) {
        out.group.set_absent();
    }
    if (!presence.tenant) {
        out.tenant.set_absent();
    }
    if (!presence.tag) {
        out.tag.set_absent();
    }
    return json::ParseStatus::Done;
}

json::Generator::Result encode_json(json::Generator &generator, const req::ConfigQueryRequest &value) noexcept {
    using detail::EncodeResult;

    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "requestId", value.request_id);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "dataId", value.data_id);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "group", value.group);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "tenant", value.tenant);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "tag", value.tag);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "notify",
                                  [&generator, &value]() noexcept { return generator.bool_value(value.notify); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module", [&generator]() noexcept {
        return detail::encode_text(generator, req::ConfigQueryRequest::kModule);
    });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

} // namespace fiber::nacos::dto
