#include "JsonCodec.h"

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
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
