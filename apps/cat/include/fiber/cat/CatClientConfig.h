#ifndef FIBER_CAT_CLIENT_CONFIG_H
#define FIBER_CAT_CLIENT_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <net/SocketAddress.h>
#include <net/TcpSocketOptions.h>

namespace fiber::cat {

enum class CatEncoderType : std::uint8_t {
    Nt1,
    Pt1,
};

struct CatRouterEndpoint {
    std::string host;
    std::uint16_t port = 8080;
};

struct CatClientConfigParams {
    std::string app_key;
    std::string hostname;
    std::string ip;
    std::string thread_group_name;
    std::string thread_id;
    std::string thread_name;
    std::vector<CatRouterEndpoint> routers;
    std::vector<net::SocketAddress> bootstrap_collectors;
};

enum class CatConfigError : std::uint8_t {
    EmptyAppKey,
    EmptyHostname,
    EmptyIp,
    EmptyServerList,
    InvalidRouter,
    InvalidCollector,
    InvalidIp,
};

class CatClientConfig {
public:
    [[nodiscard]] static std::expected<CatClientConfig, CatConfigError> create(CatClientConfigParams params);

    [[nodiscard]] const std::string &app_key() const noexcept { return params_.app_key; }
    [[nodiscard]] const std::string &hostname() const noexcept { return params_.hostname; }
    [[nodiscard]] const std::string &ip() const noexcept { return params_.ip; }
    [[nodiscard]] const std::string &thread_group_name() const noexcept { return params_.thread_group_name; }
    [[nodiscard]] const std::string &thread_id() const noexcept { return params_.thread_id; }
    [[nodiscard]] const std::string &thread_name() const noexcept { return params_.thread_name; }
    [[nodiscard]] const std::vector<CatRouterEndpoint> &routers() const noexcept { return params_.routers; }
    [[nodiscard]] const std::vector<net::SocketAddress> &bootstrap_collectors() const noexcept {
        return params_.bootstrap_collectors;
    }

private:
    explicit CatClientConfig(CatClientConfigParams params) noexcept : params_(std::move(params)) {}

    CatClientConfigParams params_;
};

struct CatClientOptions {
    double initial_sample_rate = 1.0;
    bool enable_heartbeat = true;
    bool enable_system_stats = true;
    CatEncoderType encoder = CatEncoderType::Nt1;
    std::size_t max_queued_messages = 10000;
    std::size_t max_queued_bytes = 64 * 1024 * 1024;
    std::size_t max_router_response_bytes = 64 * 1024;
    std::size_t max_collectors = 64;
    std::size_t max_batch_messages = 16;
    std::size_t max_batch_bytes = 60 * 1024;
    std::size_t max_send_bytes_per_pump = 1024 * 1024;
    std::size_t max_send_calls_per_pump = 16;
    std::size_t max_aggregation_shards = 64;
    std::size_t max_aggregate_keys_per_shard = 1024;
    std::size_t max_aggregate_key_bytes = 1024;
    std::size_t max_aggregate_bytes_per_shard = 4 * 1024 * 1024;
    std::size_t max_duration_buckets_per_key = 64;
    std::size_t problem_reserve_messages = 64;
    std::size_t problem_reserve_bytes = 1024 * 1024;
    std::size_t max_system_queued_messages = 256;
    std::size_t max_system_queued_bytes = 4 * 1024 * 1024;
    std::size_t max_heartbeat_data_bytes = 16 * 1024;
    std::size_t max_heartbeat_fields = 96;
    std::chrono::milliseconds router_connect_timeout{1000};
    std::chrono::milliseconds router_request_timeout{2000};
    std::chrono::milliseconds router_refresh_interval{180000};
    std::chrono::milliseconds collector_connect_timeout{500};
    std::chrono::milliseconds collector_write_timeout{1000};
    std::chrono::milliseconds reconnect_initial_delay{1000};
    std::chrono::milliseconds reconnect_max_delay{30000};
    std::chrono::milliseconds shutdown_drain_timeout{1000};
    std::chrono::milliseconds aggregation_flush_interval{1000};
    std::chrono::milliseconds heartbeat_interval{60000};
    std::chrono::milliseconds heartbeat_initial_delay{1000};
    net::TcpSocketOptions collector_tcp{.no_delay = net::TcpOptionMode::Enabled};
};

} // namespace fiber::cat

#endif // FIBER_CAT_CLIENT_CONFIG_H
