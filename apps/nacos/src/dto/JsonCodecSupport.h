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

} // namespace fiber::nacos::dto::detail

#endif // FIBER_NACOS_DTO_JSON_CODEC_SUPPORT_H
