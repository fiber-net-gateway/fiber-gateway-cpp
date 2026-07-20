#include <fiber/nacos/dto/JsonCodec.h>

#include "JsonCodecSupport.h"

namespace fiber::nacos::dto {
namespace {

using detail::EncodeResult;

EncodeResult encode_instance(json::Generator &generator, const NamingInstance &value) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    for (const auto &[key, field]: {std::pair{"instanceId", &value.instance_id}, std::pair{"ip", &value.ip}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    result = detail::encode_field(generator, "port", [&]() noexcept { return generator.integer(value.port); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "weight", [&]() noexcept { return generator.double_value(value.weight); });
    if (result != EncodeResult::OK) {
        return result;
    }
    for (const auto &[key, field]: {std::pair{"healthy", &value.healthy}, std::pair{"enabled", &value.enabled},
                                    std::pair{"ephemeral", &value.ephemeral}}) {
        result = detail::encode_field(generator, key, [&]() noexcept { return generator.bool_value(*field); });
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    for (const auto &[key, field]:
         {std::pair{"clusterName", &value.cluster_name}, std::pair{"serviceName", &value.service_name}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    result = detail::encode_nullable_object_field(generator, "metadata", value.metadata, [&](std::string_view text) {
        return detail::encode_text(generator, text);
    });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

EncodeResult encode_service_info(json::Generator &generator, const NamingServiceInfo &value) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    for (const auto &[key, field]: {std::pair{"name", &value.name}, std::pair{"groupName", &value.group_name},
                                    std::pair{"clusters", &value.clusters}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    result = detail::encode_field(generator, "cacheMillis",
                                  [&]() noexcept { return generator.integer(value.cache_millis); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "hosts", [&]() noexcept {
        EncodeResult array_result = generator.array_open();
        if (array_result != EncodeResult::OK) {
            return array_result;
        }
        for (const NamingInstance &host: value.hosts) {
            array_result = encode_instance(generator, host);
            if (array_result != EncodeResult::OK) {
                return array_result;
            }
        }
        return generator.array_close();
    });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "lastRefTime",
                                  [&]() noexcept { return generator.integer(value.last_ref_time); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "checksum", value.checksum);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "allIPs", [&]() noexcept { return generator.bool_value(value.all_ips); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "reachProtectionThreshold",
                                  [&]() noexcept { return generator.bool_value(value.reach_protection_threshold); });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

template<typename Response>
EncodeResult encode_response(json::Generator &generator, const Response &value) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_response_base(generator, value);
    if (result != EncodeResult::OK) {
        return result;
    }
    if constexpr (requires { value.service_info; }) {
        if (!value.service_info.is_absent()) {
            result = detail::encode_field(generator, "serviceInfo", [&]() noexcept {
                if (value.service_info.is_null()) {
                    return generator.null_value();
                }
                return encode_service_info(generator, value.service_info.value());
            });
            if (result != EncodeResult::OK) {
                return result;
            }
        }
    } else {
        result = detail::encode_nullable_text_field(generator, "type", value.type);
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    result = detail::encode_field(generator, "success",
                                  [&]() noexcept { return generator.bool_value(value.success()); });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

} // namespace

json::Generator::Result encode_json(json::Generator &generator, const req::ServiceQueryRequest &value) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_naming_request_base(generator, value);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "cluster", value.cluster);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "healthyOnly",
                                  [&]() noexcept { return generator.bool_value(value.healthy_only); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "udpPort", [&]() noexcept { return generator.integer(value.udp_port); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module",
                                  [&]() noexcept { return detail::encode_text(generator, value.kModule); });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

json::Generator::Result encode_json(json::Generator &generator, const req::SubscribeServiceRequest &value) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_naming_request_base(generator, value);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "subscribe",
                                  [&]() noexcept { return generator.bool_value(value.subscribe); });
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "clusters", value.clusters);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_field(generator, "module",
                                  [&]() noexcept { return detail::encode_text(generator, value.kModule); });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

json::Generator::Result encode_json(json::Generator &generator, const req::InstanceRequest &value) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_naming_request_base(generator, value);
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_nullable_text_field(generator, "type", value.type);
    if (result != EncodeResult::OK) {
        return result;
    }
    if (!value.instance.is_absent()) {
        result = detail::encode_field(generator, "instance", [&]() noexcept {
            return value.instance.is_null() ? generator.null_value()
                                            : encode_instance(generator, value.instance.value());
        });
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    result = detail::encode_field(generator, "module",
                                  [&]() noexcept { return detail::encode_text(generator, value.kModule); });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

json::Generator::Result encode_json(json::Generator &generator, const req::NotifySubscriberRequest &value) noexcept {
    EncodeResult result = generator.map_open();
    if (result != EncodeResult::OK) {
        return result;
    }
    result = detail::encode_request_base(generator, value);
    if (result != EncodeResult::OK) {
        return result;
    }
    for (const auto &[key, field]:
         {std::pair{"namespace", &value.namespace_id}, std::pair{"serviceName", &value.service_name},
          std::pair{"groupName", &value.group_name}}) {
        result = detail::encode_nullable_text_field(generator, key, *field);
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    if (!value.service_info.is_absent()) {
        result = detail::encode_field(generator, "serviceInfo", [&]() noexcept {
            return value.service_info.is_null() ? generator.null_value()
                                                : encode_service_info(generator, value.service_info.value());
        });
        if (result != EncodeResult::OK) {
            return result;
        }
    }
    result = detail::encode_field(generator, "module",
                                  [&]() noexcept { return detail::encode_text(generator, value.kModule); });
    if (result != EncodeResult::OK) {
        return result;
    }
    return generator.map_close();
}

json::Generator::Result encode_json(json::Generator &generator, const resp::QueryServiceResponse &value) noexcept {
    return encode_response(generator, value);
}

json::Generator::Result encode_json(json::Generator &generator, const resp::SubscribeServiceResponse &value) noexcept {
    return encode_response(generator, value);
}

json::Generator::Result encode_json(json::Generator &generator, const resp::InstanceResponse &value) noexcept {
    return encode_response(generator, value);
}

} // namespace fiber::nacos::dto
