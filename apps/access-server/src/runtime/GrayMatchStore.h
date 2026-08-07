#ifndef FIBER_ACCESS_SERVER_GRAY_MATCH_STORE_H
#define FIBER_ACCESS_SERVER_GRAY_MATCH_STORE_H

#include "../config/AccessConfig.h"
#include "../config/AccessConfigError.h"
#include "../routing/Cidr.h"
#include "../routing/ProxyAddressSelector.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/http/HttpExchange.h>

namespace fiber::access_server {

enum class GrayMatchUpdateStatus : std::uint8_t {
    IgnoredEmpty,
    Published,
};

class GrayMatchStore {
public:
    GrayMatchStore();

    [[nodiscard]] std::expected<GrayMatchUpdateStatus, AccessConfigError>
    apply(const std::optional<GrayMatchConfig> &config);

    [[nodiscard]] bool matches(const http::HttpExchange &exchange) const noexcept;
    [[nodiscard]] bool matches(std::string_view entry, std::string_view real_ip,
                               std::uint32_t random_sample) const noexcept;
    [[nodiscard]] ProxyClusterMatcher adapter() noexcept;
    [[nodiscard]] std::size_t rule_count() const noexcept;

private:
    static bool matches_request(void *context, const http::HttpExchange &exchange) noexcept;
    struct Rule {
        std::string entry;
        std::int32_t ratio = 0;
        std::vector<Cidr> cidrs;
    };

    struct Snapshot {
        std::vector<Rule> rules;
    };

    [[nodiscard]] static bool recognized_entry(std::string_view entry) noexcept;
    [[nodiscard]] std::uint32_t next_sample() const noexcept;
    [[nodiscard]] std::shared_ptr<const Snapshot> pin() const noexcept;

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const Snapshot>> published_;
#else
    std::shared_ptr<const Snapshot> published_;
#endif
    mutable std::atomic<std::uint64_t> random_sequence_{0};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_GRAY_MATCH_STORE_H
