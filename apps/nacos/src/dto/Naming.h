#ifndef FIBER_NACOS_DTO_NAMING_H
#define FIBER_NACOS_DTO_NAMING_H

#include <cstdint>
#include <string_view>

#include "Base.h"

namespace fiber::nacos::dto {

struct NamingInstance {
    NamingInstance() noexcept {
        instance_id.set_null();
        ip.set_null();
        cluster_name.set_null();
        service_name.set_null();
        metadata.set_present(json::JsonObject<std::string_view>());
    }

    json::Nullable<std::string_view> instance_id;
    json::Nullable<std::string_view> ip;
    std::int32_t port = 0;
    double weight = 1.0;
    bool healthy = true;
    bool enabled = true;
    bool ephemeral = true;
    json::Nullable<std::string_view> cluster_name;
    json::Nullable<std::string_view> service_name;
    json::Nullable<json::JsonObject<std::string_view>> metadata;
};

struct NamingServiceInfo {
    NamingServiceInfo() noexcept {
        name.set_null();
        group_name.set_null();
        clusters.set_null();
        checksum.set_present(std::string_view{});
    }

    json::Nullable<std::string_view> name;
    json::Nullable<std::string_view> group_name;
    json::Nullable<std::string_view> clusters;
    std::int64_t cache_millis = 1000;
    json::JsonArray<NamingInstance> hosts;
    std::int64_t last_ref_time = 0;
    json::Nullable<std::string_view> checksum;
    bool all_ips = false;
    bool reach_protection_threshold = false;
};

} // namespace fiber::nacos::dto

namespace fiber::nacos::dto::req {

struct ServiceQueryRequest : NamingRequestBase {
    static constexpr std::string_view kTypeName = "ServiceQueryRequest";
    static constexpr std::string_view kModule = kNamingModule;

    ServiceQueryRequest() noexcept { cluster.set_null(); }

    json::Nullable<std::string_view> cluster;
    bool healthy_only = false;
    std::int32_t udp_port = 0;
};

struct SubscribeServiceRequest : NamingRequestBase {
    static constexpr std::string_view kTypeName = "SubscribeServiceRequest";
    static constexpr std::string_view kModule = kNamingModule;

    SubscribeServiceRequest() noexcept { clusters.set_null(); }

    bool subscribe = true;
    json::Nullable<std::string_view> clusters;
};

struct InstanceRequest : NamingRequestBase {
    static constexpr std::string_view kTypeName = "InstanceRequest";
    static constexpr std::string_view kModule = kNamingModule;

    InstanceRequest() noexcept {
        type.set_null();
        instance.set_null();
    }

    json::Nullable<std::string_view> type;
    json::Nullable<NamingInstance> instance;
};

struct NotifySubscriberRequest : RequestBase {
    static constexpr std::string_view kTypeName = "NotifySubscriberRequest";
    static constexpr std::string_view kModule = kNamingModule;

    NotifySubscriberRequest() noexcept {
        namespace_id.set_null();
        service_name.set_null();
        group_name.set_null();
        service_info.set_null();
    }

    json::Nullable<std::string_view> namespace_id;
    json::Nullable<std::string_view> service_name;
    json::Nullable<std::string_view> group_name;
    json::Nullable<NamingServiceInfo> service_info;
};

} // namespace fiber::nacos::dto::req

namespace fiber::nacos::dto::resp {

struct QueryServiceResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "QueryServiceResponse";

    QueryServiceResponse() noexcept { service_info.set_null(); }

    json::Nullable<NamingServiceInfo> service_info;
};

struct SubscribeServiceResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "SubscribeServiceResponse";

    SubscribeServiceResponse() noexcept { service_info.set_null(); }

    json::Nullable<NamingServiceInfo> service_info;
};

struct InstanceResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "InstanceResponse";

    InstanceResponse() noexcept { type.set_null(); }

    json::Nullable<std::string_view> type;
};

} // namespace fiber::nacos::dto::resp

#endif // FIBER_NACOS_DTO_NAMING_H
