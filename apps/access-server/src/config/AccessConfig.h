#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::access_server {

inline constexpr std::string_view kProjectListDataId = "ploto.unified-access.projects";
inline constexpr std::string_view kProjectRouteDataIdPrefix = "ploto.unified-access.route.";
inline constexpr std::string_view kProjectRouteGroup = "ACCESS-SERVER";
inline constexpr std::string_view kGrayConfigDataId = "ploto.unified-access.gray-match";
inline constexpr std::string_view kDefaultNacosGroup = "DEFAULT_GROUP";

enum class RouteType : std::uint8_t {
    Proxy,
    Response,
};

enum class BodyType : std::uint8_t {
    Text,
    Base64,
    Template,
};

enum class HttpsStrategy : std::uint16_t {
    NotRequired = 0,
    Redirect301 = 301,
    Redirect302 = 302,
    Redirect307 = 307,
    Redirect308 = 308,
};

inline constexpr std::uint8_t kNetVdi = 1U << 0U;
inline constexpr std::uint8_t kNetOffice = 1U << 1U;
inline constexpr std::uint8_t kNetInternet = 1U << 2U;
inline constexpr std::uint8_t kNetCustom = 0;

struct HostStrategyConfig {
    // Java initializes this field to S_NOT_MUST, but an explicit null or an
    // unknown enum value replaces it with null.
    std::optional<HttpsStrategy> https = HttpsStrategy::NotRequired;
    std::uint8_t net_mask = 0;

    bool operator==(const HostStrategyConfig &) const = default;
};

struct HostConfigEntry {
    std::string pattern;
    std::optional<HostStrategyConfig> strategy;

    bool operator==(const HostConfigEntry &) const = default;
};

struct StringConfigEntry {
    std::string name;
    std::optional<std::string> value;

    bool operator==(const StringConfigEntry &) const = default;
};

using StringConfigMap = std::vector<StringConfigEntry>;
using NullableStringSet = std::vector<std::optional<std::string>>;

struct RouteBodyConfig {
    std::optional<BodyType> type;
    std::optional<std::string> content;

    bool operator==(const RouteBodyConfig &) const = default;
};

struct RouteConfig {
    std::optional<std::string> path;
    // Java initializes this field to PROXY. Explicit null and unknown enum
    // values replace it with null and are rejected later by route compilation.
    std::optional<RouteType> type = RouteType::Proxy;
    std::optional<std::string> service;
    std::optional<std::string> cluster;
    NullableStringSet addresses;
    std::optional<std::string> condition;
    StringConfigMap proxy_headers;
    StringConfigMap response_headers;
    StringConfigMap context;
    std::optional<std::string> rewrite;
    std::int32_t status = 0;
    std::optional<RouteBodyConfig> body;
    std::optional<std::int64_t> timeout_millis;
    std::optional<std::int64_t> max_client_body_size;
    std::optional<std::int64_t> max_proxy_body_size;
    std::optional<std::int64_t> websocket_timeout_millis;
    std::optional<bool> flush;
    NullableStringSet allows;

    bool operator==(const RouteConfig &) const = default;
};

struct ProjectConfig {
    std::int32_t version = 0;
    std::optional<std::vector<HostConfigEntry>> hosts;
    std::optional<std::vector<std::optional<RouteConfig>>> routes;

    bool operator==(const ProjectConfig &) const = default;
};

struct GrayMatchConfigEntry {
    std::string entry;
    std::int32_t ratio = 0;
    NullableStringSet cidrs;

    bool operator==(const GrayMatchConfigEntry &) const = default;
};

using GrayMatchConfig = std::vector<GrayMatchConfigEntry>;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_H
