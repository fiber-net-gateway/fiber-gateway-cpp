#ifndef FIBER_NACOS_DTO_INTERNAL_H
#define FIBER_NACOS_DTO_INTERNAL_H

#include <string_view>

#include "Base.h"

namespace fiber::nacos::dto::req {

struct ServerCheckRequest : InternalRequestBase {
    static constexpr std::string_view kTypeName = "ServerCheckRequest";
    static constexpr std::string_view kModule = kInternalModule;
};

struct ConnectionSetupRequest : InternalRequestBase {
    static constexpr std::string_view kTypeName = "ConnectionSetupRequest";
    static constexpr std::string_view kModule = kInternalModule;

    ConnectionSetupRequest() noexcept {
        client_version.set_null();
        tenant.set_null();
        labels.set_null();
        ability_table.set_null();
    }

    json::Nullable<std::string_view> client_version;
    json::Nullable<std::string_view> tenant;
    json::Nullable<json::JsonObject<std::string_view>> labels;
    json::Nullable<json::JsonObject<bool>> ability_table;
};

struct HealthCheckRequest : InternalRequestBase {
    static constexpr std::string_view kTypeName = "HealthCheckRequest";
    static constexpr std::string_view kModule = kInternalModule;
};

struct SetupAckRequest : InternalRequestBase {
    static constexpr std::string_view kTypeName = "SetupAckRequest";
    static constexpr std::string_view kModule = kInternalModule;

    SetupAckRequest() noexcept { ability_table.set_null(); }

    json::Nullable<json::JsonObject<bool>> ability_table;
};

struct ClientDetectionRequest : InternalRequestBase {
    static constexpr std::string_view kTypeName = "ClientDetectionRequest";
    static constexpr std::string_view kModule = kInternalModule;
};

struct ConnectResetRequest : InternalRequestBase {
    static constexpr std::string_view kTypeName = "ConnectResetRequest";
    static constexpr std::string_view kModule = kInternalModule;

    ConnectResetRequest() noexcept {
        server_ip.set_null();
        server_port.set_null();
        connection_id.set_null();
    }

    json::Nullable<std::string_view> server_ip;
    json::Nullable<std::string_view> server_port;
    json::Nullable<std::string_view> connection_id;
};

} // namespace fiber::nacos::dto::req

namespace fiber::nacos::dto::resp {

struct ServerCheckResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ServerCheckResponse";

    ServerCheckResponse() noexcept { connection_id.set_null(); }

    json::Nullable<std::string_view> connection_id;
    bool support_ability_negotiation = false;
};

struct HealthCheckResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "HealthCheckResponse";
};

struct ClientDetectionResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ClientDetectionResponse";
};

struct ConnectResetResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ConnectResetResponse";
};

struct ErrorResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "ErrorResponse";
};

} // namespace fiber::nacos::dto::resp

#endif // FIBER_NACOS_DTO_INTERNAL_H
