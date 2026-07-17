#ifndef FIBER_NACOS_DTO_CONFIG_H
#define FIBER_NACOS_DTO_CONFIG_H

#include <cstdint>
#include <string_view>

#include "Base.h"
#include "ConfigQueryRequest.h"

namespace fiber::nacos::dto::req {

struct ConfigPublishRequest : ConfigRequestBase {
    static constexpr std::string_view kTypeName = "ConfigPublishRequest";
    static constexpr std::string_view kModule = kConfigModule;

    ConfigPublishRequest() noexcept {
        content.set_null();
        cas_md5.set_null();
        addition_map.set_null();
    }

    json::Nullable<std::string_view> content;
    json::Nullable<std::string_view> cas_md5;
    json::Nullable<json::JsonObject<std::string_view>> addition_map;
};

struct ConfigRemoveRequest : ConfigRequestBase {
    static constexpr std::string_view kTypeName = "ConfigRemoveRequest";
    static constexpr std::string_view kModule = kConfigModule;

    ConfigRemoveRequest() noexcept { tag.set_null(); }

    json::Nullable<std::string_view> tag;
};

struct ConfigListenContext {
    ConfigListenContext() noexcept {
        group.set_null();
        md5.set_null();
        data_id.set_null();
        tenant.set_null();
    }

    json::Nullable<std::string_view> group;
    json::Nullable<std::string_view> md5;
    json::Nullable<std::string_view> data_id;
    json::Nullable<std::string_view> tenant;
};

struct ConfigBatchListenRequest : ConfigRequestBase {
    static constexpr std::string_view kTypeName = "ConfigBatchListenRequest";
    static constexpr std::string_view kModule = kConfigModule;

    bool listen = true;
    json::JsonArray<ConfigListenContext> config_listen_contexts;
};

struct ConfigChangeNotifyRequest : ConfigRequestBase {
    static constexpr std::string_view kTypeName = "ConfigChangeNotifyRequest";
    static constexpr std::string_view kModule = kConfigModule;
};

} // namespace fiber::nacos::dto::req

namespace fiber::nacos::dto::resp {

struct ConfigQueryResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ConfigQueryResponse";
    static constexpr std::int32_t kConfigNotFound = 300;
    static constexpr std::int32_t kConfigQueryConflict = 400;

    ConfigQueryResponse() noexcept {
        content.set_null();
        encrypted_data_key.set_null();
        content_type.set_null();
        md5.set_null();
        tag.set_null();
    }

    json::Nullable<std::string_view> content;
    json::Nullable<std::string_view> encrypted_data_key;
    json::Nullable<std::string_view> content_type;
    json::Nullable<std::string_view> md5;
    std::int64_t last_modified = 0;
    json::Nullable<std::string_view> tag;
    bool beta = false;
};

struct ConfigPublishResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ConfigPublishResponse";
};

struct ConfigRemoveResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ConfigRemoveResponse";
};

struct ConfigContext {
    ConfigContext() noexcept {
        group.set_null();
        data_id.set_null();
        tenant.set_null();
    }

    json::Nullable<std::string_view> group;
    json::Nullable<std::string_view> data_id;
    json::Nullable<std::string_view> tenant;
};

struct ConfigChangeBatchListenResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ConfigChangeBatchListenResponse";

    json::JsonArray<ConfigContext> changed_configs;
};

struct ConfigChangeNotifyResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ConfigChangeNotifyResponse";
};

} // namespace fiber::nacos::dto::resp

#endif // FIBER_NACOS_DTO_CONFIG_H
