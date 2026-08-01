#ifndef FIBER_AI_SERVER_PROVIDER_CONNECTION_MANAGER_H
#define FIBER_AI_SERVER_PROVIDER_CONNECTION_MANAGER_H

#include "../discovery/WeightedRendezvous.h"
#include "ExecutionPlan.h"
#include "ProviderEndpoint.h"
#include "WorkerDnsService.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <http/Http1ClientConnection.h>
#include <http/LocalHttp1ConnectionPoolSet.h>

namespace fiber::event {
class EventLoopGroup;
}

namespace fiber::ai_server {

enum class ProviderConnectionErrorCode : std::uint8_t {
    InvalidEndpoint,
    NoServiceEndpoint,
    Dns,
    PoolShutdown,
    Connect,
};

struct ProviderConnectionError {
    ProviderConnectionErrorCode code = ProviderConnectionErrorCode::InvalidEndpoint;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
    std::uint64_t failed_service_peer_id = 0;
    bool dns_backoff_hit = false;
};

struct ProviderServiceSelection {
    std::uint64_t rendezvous_key = 0;
    std::span<const std::uint64_t> excluded_peer_ids;
};

struct ProviderLoadBalanceLease {
    WeightedRendezvous::Selection instance;

    [[nodiscard]] bool valid() const noexcept { return instance.valid(); }
    [[nodiscard]] std::uint64_t peer_id() const noexcept { return valid() ? instance.peer_id() : 0; }
    void report(InstanceReportOutcome outcome) noexcept {
        if (!valid()) {
            return;
        }
        instance.report(outcome);
    }
};

struct ProviderConnectionLease {
    http::LocalHttp1ConnectionPoolSet::Lease lease;
    http::Http1ClientConnection *connection = nullptr;
    std::string host_header;
    std::string target;
    ProviderLoadBalanceLease load_balance;
};

class ProviderConnectionManager final : public common::NonCopyable, public common::NonMovable {
public:
    explicit ProviderConnectionManager(event::EventLoopGroup &workers) noexcept;
    ProviderConnectionManager(event::EventLoopGroup &workers, WorkerDnsService::Options dns_options) noexcept;
    ~ProviderConnectionManager();

    [[nodiscard]] async::Task<bool> init() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] async::Task<std::expected<ProviderConnectionLease, ProviderConnectionError>>
    acquire(const ResolvedProviderAttempt &attempt, std::chrono::milliseconds connect_timeout,
            ProviderServiceSelection service_selection = {}) noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    event::EventLoopGroup *workers_ = nullptr;
    WorkerDnsService dns_;
    http::LocalHttp1ConnectionPoolSet pool_;
    bool pool_initialized_ = false;
    bool initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_CONNECTION_MANAGER_H
