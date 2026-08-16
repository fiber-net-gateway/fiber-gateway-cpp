#ifndef FIBER_NACOS_NACOS_SERVICE_STATUS_H
#define FIBER_NACOS_NACOS_SERVICE_STATUS_H

#include <cstdint>

namespace fiber::nacos {

enum class NacosServicePhase : std::uint8_t {
    Created,
    Connecting,
    Ready,
    ReconnectBackoff,
    Stopping,
    Stopped,
};

enum class NacosServiceFailureCategory : std::uint8_t {
    None,
    AuthenticationUnavailable,
    Transport,
    GrpcStatus,
    Protocol,
    Server,
    Shutdown,
};

struct NacosConnectionStatus {
    // failure describes the current transition and resets to None on Ready.
    // Counters saturate at UINT64_MAX instead of wrapping.
    NacosServicePhase phase = NacosServicePhase::Created;
    NacosServiceFailureCategory failure = NacosServiceFailureCategory::None;
    bool rpc_available = false;
    std::uint64_t connection_ready_count = 0;
    std::uint64_t disconnect_count = 0;
    std::uint64_t reconnect_attempt_count = 0;

    friend bool operator==(const NacosConnectionStatus &, const NacosConnectionStatus &) = default;
};

// Counts logical subscription keys, not callback handles. registered_count and
// synchronized_count describe independent properties, while pending_count is
// the number that still needs registration or an initial synchronized value.
struct NacosSubscriptionSummary {
    std::uint64_t active_count = 0;
    std::uint64_t pending_count = 0;
    std::uint64_t registered_count = 0;
    std::uint64_t synchronized_count = 0;

    friend bool operator==(const NacosSubscriptionSummary &, const NacosSubscriptionSummary &) = default;
};

struct NacosRegistrationSummary {
    // Counts live NamingService InstanceRegistration handles. pending_count
    // includes register, update, and deregister reconciliation.
    std::uint64_t active_count = 0;
    std::uint64_t pending_count = 0;
    std::uint64_t registered_count = 0;

    friend bool operator==(const NacosRegistrationSummary &, const NacosRegistrationSummary &) = default;
};

struct ConfigServiceStatus {
    NacosConnectionStatus connection;
    NacosSubscriptionSummary subscriptions;

    friend bool operator==(const ConfigServiceStatus &, const ConfigServiceStatus &) = default;
};

struct NamingServiceStatus {
    NacosConnectionStatus connection;
    NacosSubscriptionSummary subscriptions;
    NacosRegistrationSummary registrations;

    friend bool operator==(const NamingServiceStatus &, const NamingServiceStatus &) = default;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_SERVICE_STATUS_H
