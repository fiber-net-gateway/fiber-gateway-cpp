#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
json::Generator::Result encode_json(json::Generator &generator, const resp::NotifySubscriberResponse &value) noexcept {
    using detail::EncodeResult;

    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "resultCode",
                                  [&generator, &value]() noexcept { return generator.integer(value.result_code); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "errorCode",
                                  [&generator, &value]() noexcept { return generator.integer(value.error_code); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "message", value.message);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "requestId", value.request_id);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "success",
                                  [&generator, &value]() noexcept { return generator.bool_value(value.success()); });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

} // namespace fiber::nacos::dto
