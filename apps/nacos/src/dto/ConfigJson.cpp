#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
namespace {

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

json::Generator::Result encode_json(json::Generator &generator, const req::ConfigChangeNotifyRequest &value) noexcept {
    return encode_empty_config_request(generator, value);
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
    json::Generator::Result encode_json(json::Generator &generator, const Type &value) noexcept {                      \
        return encode_empty_response(generator, value);                                                                \
    }

FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConfigPublishResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConfigRemoveResponse)
FIBER_NACOS_DEFINE_EMPTY_RESPONSE(resp::ConfigChangeNotifyResponse)

#undef FIBER_NACOS_DEFINE_EMPTY_RESPONSE

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
