#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
namespace {

struct NotifySubscriberResponsePresence {
    bool message = false;
    bool request_id = false;
};

json::ObjectFieldStatus parse_notify_subscriber_response_field(NotifySubscriberResponsePresence &presence,
                                                               std::string_view field, json::JsonParser &parser,
                                                               mem::BufPool &pool,
                                                               resp::NotifySubscriberResponse &out) noexcept {
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
        bool success = false;
        return json::to_object_field_status(json::parse_bool(parser, pool, success));
    }
    return json::ObjectFieldStatus::Unknown;
}

} // namespace

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool,
                             resp::NotifySubscriberResponse &out) noexcept {
    NotifySubscriberResponsePresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    resp::NotifySubscriberResponse &value) noexcept {
        return parse_notify_subscriber_response_field(presence, field, field_parser, field_pool, value);
    };

    const json::ParseStatus status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    if (!presence.message) {
        out.message.set_absent();
    }
    if (!presence.request_id) {
        out.request_id.set_absent();
    }
    return json::ParseStatus::Done;
}

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
