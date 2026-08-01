#ifndef FIBER_TESTS_NACOS_SNAPSHOT_TEST_BUILDER_H
#define FIBER_TESTS_NACOS_SNAPSHOT_TEST_BUILDER_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::tests {

namespace detail {

struct ConfigDataTestOwner {
    ConfigDataTestOwner(nacos::ConfigState state, std::string md5_value, std::string content_value) :
        md5(std::move(md5_value)), content(std::move(content_value)) {
        data.state = state;
        data.md5 = md5;
        data.content = content;
    }

    std::string md5;
    std::string content;
    nacos::ConfigData data;
};

struct ServiceInfoTestOwner;

} // namespace detail

struct ServiceInfoTestData {
    std::string name;
    std::string group_name;
    std::string clusters;
    std::int64_t cache_millis = 1000;
    std::vector<nacos::Instance> hosts;
    std::int64_t last_ref_time = 0;
    std::string checksum;
    bool all_ips = false;
    bool reach_protection_threshold = false;
};

namespace detail {

struct ServiceInfoTestOwner {
    explicit ServiceInfoTestOwner(ServiceInfoTestData value) : input(std::move(value)) {
        metadata.resize(input.hosts.size());
        hosts.reserve(input.hosts.size());
        for (std::size_t i = 0; i < input.hosts.size(); ++i) {
            const nacos::Instance &source = input.hosts[i];
            auto &metadata_view = metadata[i];
            metadata_view.reserve(source.metadata.size());
            for (const nacos::NamingMetadataEntry &entry: source.metadata) {
                metadata_view.push_back({.key = entry.key, .value = entry.value});
            }
            hosts.push_back(nacos::ServiceInstance{
                    .instance_id = source.instance_id,
                    .ip = source.ip,
                    .port = source.port,
                    .weight = source.weight,
                    .healthy = source.healthy,
                    .enabled = source.enabled,
                    .ephemeral = source.ephemeral,
                    .cluster_name = source.cluster_name,
                    .service_name = source.service_name,
                    .metadata = metadata_view,
            });
        }
        data.name = input.name;
        data.group_name = input.group_name;
        data.clusters = input.clusters;
        data.cache_millis = input.cache_millis;
        data.hosts = hosts;
        data.last_ref_time = input.last_ref_time;
        data.checksum = input.checksum;
        data.all_ips = input.all_ips;
        data.reach_protection_threshold = input.reach_protection_threshold;
    }

    ServiceInfoTestData input;
    std::vector<std::vector<nacos::ServiceMetadataEntry>> metadata;
    std::vector<nacos::ServiceInstance> hosts;
    nacos::ServiceInfo data;
};

} // namespace detail

[[nodiscard]] inline std::shared_ptr<const nacos::ConfigData>
make_config_data(nacos::ConfigState state, std::string md5 = {}, std::string content = {}) {
    auto owner = std::make_shared<detail::ConfigDataTestOwner>(state, std::move(md5), std::move(content));
    return std::shared_ptr<const nacos::ConfigData>(owner, &owner->data);
}

[[nodiscard]] inline std::shared_ptr<const nacos::ServiceInfo> make_service_info(ServiceInfoTestData value) {
    auto owner = std::make_shared<detail::ServiceInfoTestOwner>(std::move(value));
    return std::shared_ptr<const nacos::ServiceInfo>(owner, &owner->data);
}

} // namespace fiber::tests

#endif // FIBER_TESTS_NACOS_SNAPSHOT_TEST_BUILDER_H
