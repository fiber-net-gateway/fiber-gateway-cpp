#include <fiber/nacos/discovery/ServiceDiscovery.h>

#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <utility>

#include <common/Assert.h>

namespace fiber::nacos {
namespace {

std::string make_authority(std::string_view host, std::uint16_t port, const std::optional<net::IpAddress> &ip) {
    std::array<char, 5> port_text{};
    const auto converted = std::to_chars(port_text.data(), port_text.data() + port_text.size(), port);
    std::string result;
    result.reserve(host.size() + 8);
    if (ip && ip->is_v6()) {
        result.push_back('[');
        result.append(host);
        result.push_back(']');
    } else {
        result.append(host);
    }
    result.push_back(':');
    result.append(port_text.data(), converted.ptr);
    return result;
}

} // namespace

LoadBalancerOps::CreateResult LoadBalancerOps::create(std::string_view service_name, std::string_view group,
                                                      const std::shared_ptr<const ServiceInfo> &snapshot) {
    FIBER_ASSERT(snapshot != nullptr);
    auto state = std::make_shared<State>(options_.load_balancer);
    const UpdateResult result = state->update_instances(make_update(service_name, group, *snapshot));
    FIBER_ASSERT(result == UpdateResult::Applied);
    return CreateResult{.state = std::move(state), .result = result};
}

LoadBalancerOps::UpdateResult LoadBalancerOps::update(State &state, std::string_view service_name,
                                                      std::string_view group,
                                                      const std::shared_ptr<const ServiceInfo> &snapshot) {
    FIBER_ASSERT(snapshot != nullptr);
    return state.update_instances(make_update(service_name, group, *snapshot));
}

DiscoveredService LoadBalancerOps::make_update(std::string_view service_name, std::string_view group,
                                               const ServiceInfo &info) const {
    DiscoveredService update;
    update.service_name = service_name;
    update.group = info.group_name.empty() ? std::string(group) : std::string(info.group_name);
    update.checksum = info.checksum;
    update.last_ref_time = info.last_ref_time;
    update.instances.reserve(info.hosts.size());
    for (const ServiceInstance &instance: info.hosts) {
        if (!instance.enabled || !instance.healthy || !std::isfinite(instance.weight) || instance.weight <= 0.0 ||
            instance.ip.empty() || instance.port == 0) {
            continue;
        }
        net::IpAddress ip;
        const bool parsed_ip = net::IpAddress::parse(instance.ip, ip);
        if (options_.require_ip && !parsed_ip) {
            continue;
        }
        update.instances.push_back(DiscoveredInstance{
                .instance_id = std::string(instance.instance_id),
                .host = std::string(instance.ip),
                .ip_address = parsed_ip ? std::optional(ip) : std::nullopt,
                .port = instance.port,
                .authority = make_authority(instance.ip, instance.port, parsed_ip ? std::optional(ip) : std::nullopt),
                .weight = instance.weight,
                .cluster_name = std::string(instance.cluster_name),
        });
    }
    return update;
}

} // namespace fiber::nacos
