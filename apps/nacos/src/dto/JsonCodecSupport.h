#ifndef FIBER_NACOS_DTO_JSON_CODEC_SUPPORT_H
#define FIBER_NACOS_DTO_JSON_CODEC_SUPPORT_H

#include <string_view>
#include <utility>

#include <common/json/JsonEncode.h>
#include <common/json/JsonValue.h>

namespace fiber::nacos::dto::detail {

using EncodeResult = json::Generator::Result;

[[nodiscard]] inline EncodeResult encode_key(json::Generator &generator, std::string_view key) noexcept {
    return generator.string(key.data() ? key.data() : "", key.size());
}

template<typename Encoder>
[[nodiscard]] EncodeResult encode_field(json::Generator &generator, std::string_view key, Encoder &&encoder) noexcept {
    EncodeResult result = encode_key(generator, key);
    if (result != EncodeResult::OK) {
        return result;
    }
    return std::forward<Encoder>(encoder)();
}

[[nodiscard]] inline EncodeResult encode_text(json::Generator &generator, std::string_view value) noexcept {
    return generator.string(value.data() ? value.data() : "", value.size());
}

[[nodiscard]] inline EncodeResult encode_nullable_text_field(json::Generator &generator, std::string_view key,
                                                             const json::Nullable<std::string_view> &value) noexcept {
    if (value.is_absent()) {
        return EncodeResult::OK;
    }
    return encode_field(generator, key, [&generator, &value]() noexcept {
        if (value.is_null()) {
            return generator.null_value();
        }
        return encode_text(generator, value.value());
    });
}

template<typename T, typename ValueEncoder>
[[nodiscard]] EncodeResult encode_object(json::Generator &generator, const json::JsonObject<T> &object,
                                         ValueEncoder &&value_encoder) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    for (const auto &entry: object) {
        result = encode_key(generator, entry.key);
        if (result != EncodeResult::OK) {
            return result;
        }
        result = std::forward<ValueEncoder>(value_encoder)(entry.value);
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    return generator.map_close();
}

template<typename T, typename ValueEncoder>
[[nodiscard]] EncodeResult encode_nullable_object_field(json::Generator &generator, std::string_view key,
                                                        const json::Nullable<json::JsonObject<T>> &value,
                                                        ValueEncoder &&value_encoder) noexcept {
    if (value.is_absent()) {
        return EncodeResult::OK;
    }
    return encode_field(generator, key, [&]() noexcept {
        if (value.is_null()) {
            return generator.null_value();
        }
        return encode_object(generator, value.value(), std::forward<ValueEncoder>(value_encoder));
    });
}

[[nodiscard]] inline EncodeResult encode_request_base(json::Generator &generator, const RequestBase &value) noexcept {
    return encode_nullable_text_field(generator, "requestId", value.request_id);
}

[[nodiscard]] inline EncodeResult encode_config_request_base(json::Generator &generator,
                                                             const ConfigRequestBase &value) noexcept {
    EncodeResult result = encode_request_base(generator, value);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = encode_nullable_text_field(generator, "dataId", value.data_id);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = encode_nullable_text_field(generator, "group", value.group);
    if (result != EncodeResult::OK) {
        return result;
    }
    return encode_nullable_text_field(generator, "tenant", value.tenant);
}

[[nodiscard]] inline EncodeResult encode_response_base(json::Generator &generator, const ResponseBase &value) noexcept {
    EncodeResult result =
            encode_field(generator, "resultCode", [&]() noexcept { return generator.integer(value.result_code); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = encode_field(generator, "errorCode", [&]() noexcept { return generator.integer(value.error_code); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = encode_nullable_text_field(generator, "message", value.message);
    if (result != EncodeResult::OK) {
        return result;
    }
    return encode_nullable_text_field(generator, "requestId", value.request_id);
}

struct RequestPresence {
    bool request_id = false;
};

struct ConfigRequestPresence : RequestPresence {
    bool data_id = false;
    bool group = false;
    bool tenant = false;
};

struct ResponsePresence {
    bool message = false;
    bool request_id = false;
};

[[nodiscard]] inline json::ObjectFieldStatus parse_request_base_field(RequestPresence &presence, std::string_view field,
                                                                      json::JsonParser &parser, mem::BufPool &pool,
                                                                      RequestBase &out) noexcept {
    if (field != "requestId") {
        return json::ObjectFieldStatus::Unknown;
    }
    presence.request_id = true;
    return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.request_id));
}

[[nodiscard]] inline json::ObjectFieldStatus
parse_config_request_base_field(ConfigRequestPresence &presence, std::string_view field, json::JsonParser &parser,
                                mem::BufPool &pool, ConfigRequestBase &out) noexcept {
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
    return json::ObjectFieldStatus::Unknown;
}

[[nodiscard]] inline json::ObjectFieldStatus parse_response_base_field(ResponsePresence &presence,
                                                                       std::string_view field, json::JsonParser &parser,
                                                                       mem::BufPool &pool, ResponseBase &out) noexcept {
    if (field == "resultCode") {
        return json::to_object_field_status(json::parse_integral<std::int32_t>(parser, pool, out.result_code));
    }
    if (field == "errorCode") {
        return json::to_object_field_status(json::parse_integral<std::int32_t>(parser, pool, out.error_code));
    }
    if (field == "message") {
        presence.message = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.message));
    }
    if (field == "requestId") {
        presence.request_id = true;
        return json::to_object_field_status(json::parse_nullable<json::parse_text>(parser, pool, out.request_id));
    }
    if (field == "success") {
        bool ignored = false;
        return json::to_object_field_status(json::parse_bool(parser, pool, ignored));
    }
    return json::ObjectFieldStatus::Unknown;
}

inline void finish_presence(const RequestPresence &presence, RequestBase &out) noexcept {
    if (!presence.request_id) {
        out.request_id.set_absent();
    }
}

inline void finish_presence(const ConfigRequestPresence &presence, ConfigRequestBase &out) noexcept {
    finish_presence(static_cast<const RequestPresence &>(presence), out);
    if (!presence.data_id) {
        out.data_id.set_absent();
    }
    if (!presence.group) {
        out.group.set_absent();
    }
    if (!presence.tenant) {
        out.tenant.set_absent();
    }
}

inline void finish_presence(const ResponsePresence &presence, ResponseBase &out) noexcept {
    if (!presence.message) {
        out.message.set_absent();
    }
    if (!presence.request_id) {
        out.request_id.set_absent();
    }
}

[[nodiscard]] inline json::ObjectFieldStatus parse_module(std::string_view expected, json::JsonParser &parser,
                                                          mem::BufPool &pool) noexcept {
    std::string_view module;
    if (json::parse_text(parser, pool, module) != json::ParseStatus::Done) {
        return json::ObjectFieldStatus::Error;
    }
    if (module != expected) {
        (void) parser.fail("unexpected Nacos request module");
        return json::ObjectFieldStatus::Error;
    }
    return json::ObjectFieldStatus::Parsed;
}

} // namespace fiber::nacos::dto::detail

#endif // FIBER_NACOS_DTO_JSON_CODEC_SUPPORT_H
