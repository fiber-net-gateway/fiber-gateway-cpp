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

} // namespace fiber::nacos::dto::detail

#endif // FIBER_NACOS_DTO_JSON_CODEC_SUPPORT_H
