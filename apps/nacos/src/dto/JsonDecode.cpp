#include <fiber/nacos/dto/JsonCodec.h>

#include <common/json/JsonStructDecode.h>

FIBER_JSON_STRUCT(fiber::nacos::dto::RequestBase, FIBER_JSON_NAMED_OPTIONAL_FIELD(request_id, "requestId"));

FIBER_JSON_STRUCT(fiber::nacos::dto::ConfigRequestBase, FIBER_JSON_BASE(fiber::nacos::dto::RequestBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(data_id, "dataId"), FIBER_JSON_OPTIONAL_FIELD(group),
                  FIBER_JSON_OPTIONAL_FIELD(tenant));

FIBER_JSON_STRUCT(fiber::nacos::dto::NamingRequestBase, FIBER_JSON_BASE(fiber::nacos::dto::RequestBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(namespace_id, "namespace"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(service_name, "serviceName"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(group_name, "groupName"));

FIBER_JSON_STRUCT(fiber::nacos::dto::InternalRequestBase, FIBER_JSON_BASE(fiber::nacos::dto::RequestBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::ResponseBase, FIBER_JSON_NAMED_OPTIONAL_FIELD(result_code, "resultCode"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(error_code, "errorCode"), FIBER_JSON_OPTIONAL_FIELD(message),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(request_id, "requestId"),
                  FIBER_JSON_OPTIONAL_IGNORED(bool, "success"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConfigListenContext, FIBER_JSON_OPTIONAL_FIELD(group),
                  FIBER_JSON_OPTIONAL_FIELD(md5), FIBER_JSON_NAMED_OPTIONAL_FIELD(data_id, "dataId"),
                  FIBER_JSON_OPTIONAL_FIELD(tenant));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ConfigContext, FIBER_JSON_OPTIONAL_FIELD(group),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(data_id, "dataId"), FIBER_JSON_OPTIONAL_FIELD(tenant));

FIBER_JSON_STRUCT(fiber::nacos::dto::NamingInstance, FIBER_JSON_NAMED_OPTIONAL_FIELD(instance_id, "instanceId"),
                  FIBER_JSON_OPTIONAL_FIELD(ip), FIBER_JSON_OPTIONAL_FIELD(port), FIBER_JSON_OPTIONAL_FIELD(weight),
                  FIBER_JSON_OPTIONAL_FIELD(healthy), FIBER_JSON_OPTIONAL_FIELD(enabled),
                  FIBER_JSON_OPTIONAL_FIELD(ephemeral), FIBER_JSON_NAMED_OPTIONAL_FIELD(cluster_name, "clusterName"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(service_name, "serviceName"), FIBER_JSON_OPTIONAL_FIELD(metadata));

FIBER_JSON_STRUCT(fiber::nacos::dto::NamingServiceInfo, FIBER_JSON_OPTIONAL_FIELD(name),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(group_name, "groupName"), FIBER_JSON_OPTIONAL_FIELD(clusters),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(cache_millis, "cacheMillis"), FIBER_JSON_OPTIONAL_FIELD(hosts),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(last_ref_time, "lastRefTime"), FIBER_JSON_OPTIONAL_FIELD(checksum),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(all_ips, "allIPs"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(reach_protection_threshold, "reachProtectionThreshold"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ServerCheckRequest, FIBER_JSON_BASE(fiber::nacos::dto::InternalRequestBase),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConnectionSetupRequest,
                  FIBER_JSON_BASE(fiber::nacos::dto::InternalRequestBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(client_version, "clientVersion"), FIBER_JSON_OPTIONAL_FIELD(tenant),
                  FIBER_JSON_OPTIONAL_FIELD(labels), FIBER_JSON_NAMED_OPTIONAL_FIELD(ability_table, "abilityTable"),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::HealthCheckRequest, FIBER_JSON_BASE(fiber::nacos::dto::InternalRequestBase),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::SetupAckRequest, FIBER_JSON_BASE(fiber::nacos::dto::InternalRequestBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(ability_table, "abilityTable"),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ClientDetectionRequest,
                  FIBER_JSON_BASE(fiber::nacos::dto::InternalRequestBase),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConnectResetRequest, FIBER_JSON_BASE(fiber::nacos::dto::InternalRequestBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(server_ip, "serverIp"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(server_port, "serverPort"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(connection_id, "connectionId"),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConfigQueryRequest, FIBER_JSON_BASE(fiber::nacos::dto::ConfigRequestBase),
                  FIBER_JSON_OPTIONAL_FIELD(tag), FIBER_JSON_OPTIONAL_FIELD(notify),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConfigPublishRequest, FIBER_JSON_BASE(fiber::nacos::dto::ConfigRequestBase),
                  FIBER_JSON_OPTIONAL_FIELD(content), FIBER_JSON_NAMED_OPTIONAL_FIELD(cas_md5, "casMd5"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(addition_map, "additionMap"),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConfigRemoveRequest, FIBER_JSON_BASE(fiber::nacos::dto::ConfigRequestBase),
                  FIBER_JSON_OPTIONAL_FIELD(tag),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConfigBatchListenRequest,
                  FIBER_JSON_BASE(fiber::nacos::dto::ConfigRequestBase), FIBER_JSON_OPTIONAL_FIELD(listen),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(config_listen_contexts, "configListenContexts"),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ConfigChangeNotifyRequest,
                  FIBER_JSON_BASE(fiber::nacos::dto::ConfigRequestBase),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::ServiceQueryRequest, FIBER_JSON_BASE(fiber::nacos::dto::NamingRequestBase),
                  FIBER_JSON_OPTIONAL_FIELD(cluster), FIBER_JSON_NAMED_OPTIONAL_FIELD(healthy_only, "healthyOnly"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(udp_port, "udpPort"),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::SubscribeServiceRequest,
                  FIBER_JSON_BASE(fiber::nacos::dto::NamingRequestBase), FIBER_JSON_OPTIONAL_FIELD(subscribe),
                  FIBER_JSON_OPTIONAL_FIELD(clusters),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::InstanceRequest, FIBER_JSON_BASE(fiber::nacos::dto::NamingRequestBase),
                  FIBER_JSON_OPTIONAL_FIELD(type), FIBER_JSON_OPTIONAL_FIELD(instance),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::req::NotifySubscriberRequest, FIBER_JSON_BASE(fiber::nacos::dto::RequestBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(namespace_id, "namespace"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(service_name, "serviceName"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(group_name, "groupName"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(service_info, "serviceInfo"),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", Self::kModule, "unexpected Nacos request module"));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ServerCheckResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(connection_id, "connectionId"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(support_ability_negotiation, "supportAbilityNegotiation"));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::HealthCheckResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ClientDetectionResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ConnectResetResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ErrorResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::NotifySubscriberResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::AuthTokenResponse,
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(access_token, "accessToken"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(token_ttl, "tokenTtl"));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ConfigQueryResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase),
                  FIBER_JSON_OPTIONAL_FIELD(content),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(encrypted_data_key, "encryptedDataKey"),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(content_type, "contentType"), FIBER_JSON_OPTIONAL_FIELD(md5),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(last_modified, "lastModified"), FIBER_JSON_OPTIONAL_FIELD(tag),
                  FIBER_JSON_OPTIONAL_FIELD(beta));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ConfigPublishResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ConfigRemoveResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ConfigChangeBatchListenResponse,
                  FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(changed_configs, "changedConfigs"));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::ConfigChangeNotifyResponse,
                  FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::QueryServiceResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(service_info, "serviceInfo"));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::SubscribeServiceResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase),
                  FIBER_JSON_NAMED_OPTIONAL_FIELD(service_info, "serviceInfo"));

FIBER_JSON_STRUCT(fiber::nacos::dto::resp::InstanceResponse, FIBER_JSON_BASE(fiber::nacos::dto::ResponseBase),
                  FIBER_JSON_OPTIONAL_FIELD(type));

namespace fiber::nacos::dto {

#define FIBER_NACOS_DEFINE_JSON_PARSE(Type)                                                                            \
    json::ParseStatus parse_json(json::JsonParser &parser, mem::BufPool &pool, Type &out) noexcept {                   \
        return json::parse_value(parser, pool, out);                                                                   \
    }

FIBER_NACOS_DEFINE_JSON_PARSE(RequestBase)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ServerCheckRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ConnectionSetupRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::HealthCheckRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::SetupAckRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ClientDetectionRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ConnectResetRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ConfigQueryRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ConfigPublishRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ConfigRemoveRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ConfigBatchListenRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ConfigChangeNotifyRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::ServiceQueryRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::SubscribeServiceRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::InstanceRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(req::NotifySubscriberRequest)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ServerCheckResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::HealthCheckResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ClientDetectionResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ConnectResetResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ErrorResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::NotifySubscriberResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::AuthTokenResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ConfigQueryResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ConfigPublishResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ConfigRemoveResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ConfigChangeBatchListenResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::ConfigChangeNotifyResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::QueryServiceResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::SubscribeServiceResponse)
FIBER_NACOS_DEFINE_JSON_PARSE(resp::InstanceResponse)

#undef FIBER_NACOS_DEFINE_JSON_PARSE

} // namespace fiber::nacos::dto
