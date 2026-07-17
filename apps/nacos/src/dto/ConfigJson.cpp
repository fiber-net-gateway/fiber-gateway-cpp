#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
namespace {

json::ParseStatus parse_string_object(json::JsonParser &parser, mem::BufPool &pool,
                                      json::JsonObject<std::string_view> &out) noexcept {
    return json::parse_object<json::parse_text>(parser, pool, out);
}

template<typename T>
json::ParseStatus parse_empty_config_request(json::JsonParser &parser, mem::BufPool &pool, T &out) noexcept {
    detail::ConfigRequestPresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    T &value) noexcept {
        auto status = detail::parse_config_request_base_field(presence, field, field_parser, field_pool, value);
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
json::Generator::Result encode_empty_config_request(json::Generator &generator, const T &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_config_request_base(generator, value);
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

json::ParseStatus parse_config_listen_context(json::JsonParser &parser, mem::BufPool &pool,
                                              req::ConfigListenContext &out) noexcept {
    struct Presence {
        bool group = false;
        bool md5 = false;
        bool data_id = false;
        bool tenant = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::ConfigListenContext &value) noexcept {
        json::Nullable<std::string_view> *target = nullptr;
        bool *present = nullptr;
        if (field == "group") {
            target = &value.group;
            present = &presence.group;
        } else if (field == "md5") {
            target = &value.md5;
            present = &presence.md5;
        } else if (field == "dataId") {
            target = &value.data_id;
            present = &presence.data_id;
        } else if (field == "tenant") {
            target = &value.tenant;
            present = &presence.tenant;
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
    if (!presence.group) {
        out.group.set_absent();
    }
    if (!presence.md5) {
        out.md5.set_absent();
    }
    if (!presence.data_id) {
        out.data_id.set_absent();
    }
    if (!presence.tenant) {
        out.tenant.set_absent();
    }
    return json::ParseStatus::Done;
}

json::Generator::Result encode_config_listen_context(json::Generator &generator,
                                                     const req::ConfigListenContext &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    for (const auto &[key, field]: {std::pair{"group", &value.group}, std::pair{"md5", &value.md5},
                                    std::pair{"dataId", &value.data_id}, std::pair{"tenant", &value.tenant}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != json::Generator::Result::OK) {
            return result;
        }
    }
    return generator.map_close();
}

json::ParseStatus parse_config_context(json::JsonParser &parser, mem::BufPool &pool,
                                       resp::ConfigContext &out) noexcept {
    struct Presence {
        bool group = false;
        bool data_id = false;
        bool tenant = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    resp::ConfigContext &value) noexcept {
        json::Nullable<std::string_view> *target = nullptr;
        bool *present = nullptr;
        if (field == "group") {
            target = &value.group;
            present = &presence.group;
        } else if (field == "dataId") {
            target = &value.data_id;
            present = &presence.data_id;
        } else if (field == "tenant") {
            target = &value.tenant;
            present = &presence.tenant;
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
    if (!presence.group) {
        out.group.set_absent();
    }
    if (!presence.data_id) {
        out.data_id.set_absent();
    }
    if (!presence.tenant) {
        out.tenant.set_absent();
    }
    return json::ParseStatus::Done;
}

json::Generator::Result encode_config_context(json::Generator &generator, const resp::ConfigContext &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    for (const auto &[key, field]:
         {std::pair{"group", &value.group}, std::pair{"dataId", &value.data_id}, std::pair{"tenant", &value.tenant}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != json::Generator::Result::OK) {
            return result;
        }
    }
    return generator.map_close();
}

} // namespace

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, req::ConfigPublishRequest &out) noexcept {
    struct Presence : detail::ConfigRequestPresence {
        bool content = false;
        bool cas_md5 = false;
        bool addition_map = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::ConfigPublishRequest &value) noexcept {
        auto status = detail::parse_config_request_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "content") {
            presence.content = true;
            return json::to_object_field_status(
                    json::parse_nullable<json::parse_text>(field_parser, field_pool, value.content));
        }
        if (field == "casMd5") {
            presence.cas_md5 = true;
            return json::to_object_field_status(
                    json::parse_nullable<json::parse_text>(field_parser, field_pool, value.cas_md5));
        }
        if (field == "additionMap") {
            presence.addition_map = true;
            return json::to_object_field_status(
                    json::parse_nullable<parse_string_object>(field_parser, field_pool, value.addition_map));
        }
        if (field == "module") {
            return detail::parse_module(req::ConfigPublishRequest::kModule, field_parser, field_pool);
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    detail::finish_presence(presence, out);
    if (!presence.content) {
        out.content.set_absent();
    }
    if (!presence.cas_md5) {
        out.cas_md5.set_absent();
    }
    if (!presence.addition_map) {
        out.addition_map.set_absent();
    }
    return json::ParseStatus::Done;
}

json::Generator::Result encode_json(json::Generator &generator, const req::ConfigPublishRequest &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_config_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "content", value.content);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "casMd5", value.cas_md5);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_object_field(
            generator, "additionMap", value.addition_map,
            [&](std::string_view text) { return detail::encode_text(generator, text); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module", [&]() noexcept {
        return detail::encode_text(generator, req::ConfigPublishRequest::kModule);
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, req::ConfigRemoveRequest &out) noexcept {
    struct Presence : detail::ConfigRequestPresence {
        bool tag = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::ConfigRemoveRequest &value) noexcept {
        auto status = detail::parse_config_request_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "tag") {
            presence.tag = true;
            return json::to_object_field_status(
                    json::parse_nullable<json::parse_text>(field_parser, field_pool, value.tag));
        }
        if (field == "module") {
            return detail::parse_module(req::ConfigRemoveRequest::kModule, field_parser, field_pool);
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    detail::finish_presence(presence, out);
    if (!presence.tag) {
        out.tag.set_absent();
    }
    return json::ParseStatus::Done;
}

json::Generator::Result encode_json(json::Generator &generator, const req::ConfigRemoveRequest &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_config_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "tag", value.tag);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module", [&]() noexcept {
        return detail::encode_text(generator, req::ConfigRemoveRequest::kModule);
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool,
                             req::ConfigBatchListenRequest &out) noexcept {
    detail::ConfigRequestPresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    req::ConfigBatchListenRequest &value) noexcept {
        auto status = detail::parse_config_request_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "listen") {
            return json::to_object_field_status(json::parse_bool(field_parser, field_pool, value.listen));
        }
        if (field == "configListenContexts") {
            return json::to_object_field_status(json::parse_array<parse_config_listen_context>(
                    field_parser, field_pool, value.config_listen_contexts));
        }
        if (field == "module") {
            return detail::parse_module(req::ConfigBatchListenRequest::kModule, field_parser, field_pool);
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status == json::ParseStatus::Done) {
        detail::finish_presence(presence, out);
    }
    return status;
}

json::Generator::Result encode_json(json::Generator &generator, const req::ConfigBatchListenRequest &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_config_request_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "listen", [&]() noexcept { return generator.bool_value(value.listen); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "configListenContexts", [&]() noexcept {
        auto array_result = generator.array_open();
        if (array_result != json::Generator::Result::OK) {
            return array_result;
        }
        for (const auto &context: value.config_listen_contexts) {
            array_result = encode_config_listen_context(generator, context);
            if (array_result != json::Generator::Result::OK) {
                return array_result;
            }
        }
        return generator.array_close();
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module", [&]() noexcept {
        return detail::encode_text(generator, req::ConfigBatchListenRequest::kModule);
    });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    return generator.map_close();
}

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool,
                             req::ConfigChangeNotifyRequest &out) noexcept {
    return parse_empty_config_request(parser, pool, out);
}

json::Generator::Result encode_json(json::Generator &generator, const req::ConfigChangeNotifyRequest &value) noexcept {
    return encode_empty_config_request(generator, value);
}

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, resp::ConfigQueryResponse &out) noexcept {
    struct Presence : detail::ResponsePresence {
        bool content = false;
        bool encrypted_data_key = false;
        bool content_type = false;
        bool md5 = false;
        bool tag = false;
    } presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    resp::ConfigQueryResponse &value) noexcept {
        auto status = detail::parse_response_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        json::Nullable<std::string_view> *text_target = nullptr;
        bool *present = nullptr;
        if (field == "content") {
            text_target = &value.content;
            present = &presence.content;
        } else if (field == "encryptedDataKey") {
            text_target = &value.encrypted_data_key;
            present = &presence.encrypted_data_key;
        } else if (field == "contentType") {
            text_target = &value.content_type;
            present = &presence.content_type;
        } else if (field == "md5") {
            text_target = &value.md5;
            present = &presence.md5;
        } else if (field == "tag") {
            text_target = &value.tag;
            present = &presence.tag;
        } else if (field == "lastModified") {
            return json::to_object_field_status(
                    json::parse_integral<std::int64_t>(field_parser, field_pool, value.last_modified));
        } else if (field == "beta") {
            return json::to_object_field_status(json::parse_bool(field_parser, field_pool, value.beta));
        } else {
            return json::ObjectFieldStatus::Unknown;
        }
        *present = true;
        return json::to_object_field_status(
                json::parse_nullable<json::parse_text>(field_parser, field_pool, *text_target));
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status != json::ParseStatus::Done) {
        return status;
    }
    detail::finish_presence(presence, out);
    if (!presence.content) {
        out.content.set_absent();
    }
    if (!presence.encrypted_data_key) {
        out.encrypted_data_key.set_absent();
    }
    if (!presence.content_type) {
        out.content_type.set_absent();
    }
    if (!presence.md5) {
        out.md5.set_absent();
    }
    if (!presence.tag) {
        out.tag.set_absent();
    }
    return json::ParseStatus::Done;
}

json::Generator::Result encode_json(json::Generator &generator, const resp::ConfigQueryResponse &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_response_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    for (const auto &[key, field]:
         {std::pair{"content", &value.content}, std::pair{"encryptedDataKey", &value.encrypted_data_key},
          std::pair{"contentType", &value.content_type}, std::pair{"md5", &value.md5}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != json::Generator::Result::OK) {
            return result;
        }
    }
    result = detail::encode_field(generator, "lastModified",
                                  [&]() noexcept { return generator.integer(value.last_modified); });
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "tag", value.tag);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "beta", [&]() noexcept { return generator.bool_value(value.beta); });
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

FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConfigPublishResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConfigRemoveResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConfigChangeNotifyResponse)

#undef FIBER_NACOS_DEFINE_EMPTY_RESPONSE

json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool,
                             resp::ConfigChangeBatchListenResponse &out) noexcept {
    detail::ResponsePresence presence;
    auto field_parser = [&presence](std::string_view field, json::JsonParser &field_parser, mem::BufPool &field_pool,
                                    resp::ConfigChangeBatchListenResponse &value) noexcept {
        auto status = detail::parse_response_base_field(presence, field, field_parser, field_pool, value);
        if (status != json::ObjectFieldStatus::Unknown) {
            return status;
        }
        if (field == "changedConfigs") {
            return json::to_object_field_status(
                    json::parse_array<parse_config_context>(field_parser, field_pool, value.changed_configs));
        }
        return json::ObjectFieldStatus::Unknown;
    };
    const auto status = json::parse_object_fields(parser, pool, out, field_parser);
    if (status == json::ParseStatus::Done) {
        detail::finish_presence(presence, out);
    }
    return status;
}

json::Generator::Result encode_json(json::Generator &generator,
                                    const resp::ConfigChangeBatchListenResponse &value) noexcept {
    auto result = generator.map_open();
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_response_base(generator, value);
    if (result != json::Generator::Result::OK) {
        return result;
    }
    result = detail::encode_field(generator, "changedConfigs", [&]() noexcept {
        auto array_result = generator.array_open();
        if (array_result != json::Generator::Result::OK) {
            return array_result;
        }
        for (const auto &context: value.changed_configs) {
            array_result = encode_config_context(generator, context);
            if (array_result != json::Generator::Result::OK) {
                return array_result;
            }
        }
        return generator.array_close();
    });
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

} // namespace fiber::nacos::dto
