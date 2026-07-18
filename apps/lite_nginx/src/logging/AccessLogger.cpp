#include "AccessLogger.h"

#include <arpa/inet.h>

#include <array>
#include <atomic>
#include <chrono>
#include <string_view>

#include "http/HttpExchange.h"
#include "log/Log.h"

namespace fiber::lite_nginx::logging {
namespace {

DEFINE_LOGGER(LOG_ACCESS, "lite_nginx.access");

std::atomic<std::uint64_t> g_next_request_id{1};

std::string_view protocol_name(fiber::http::HttpVersion version) noexcept {
    switch (version) {
        case fiber::http::HttpVersion::HTTP_0_9:
            return "HTTP/0.9";
        case fiber::http::HttpVersion::HTTP_1_0:
            return "HTTP/1.0";
        case fiber::http::HttpVersion::HTTP_1_1:
            return "HTTP/1.1";
        case fiber::http::HttpVersion::HTTP_2_0:
            return "HTTP/2";
        case fiber::http::HttpVersion::HTTP_3_0:
            return "HTTP/3";
    }
    return "unknown";
}

std::string_view remote_ip(const fiber::net::IpAddress &ip, std::array<char, INET6_ADDRSTRLEN> &buffer) noexcept {
    const void *source = nullptr;
    int family = AF_INET;
    std::array<std::uint8_t, fiber::net::IpAddress::kV4Size> v4{};
    if (ip.is_v4()) {
        v4 = ip.v4_bytes();
        source = v4.data();
    } else {
        family = AF_INET6;
        source = ip.v6_bytes().data();
    }
    const char *result = ::inet_ntop(family, source, buffer.data(), static_cast<socklen_t>(buffer.size()));
    return result ? std::string_view(result) : std::string_view("-");
}

std::string_view access_outcome(const fiber::http::HttpResponseStats &stats,
                                const RequestLogContext &context) noexcept {
    if (context.upstream_error != fiber::common::IoErr::None) {
        return "upstream_error";
    }
    if (stats.terminal_error != fiber::common::IoErr::None) {
        return "io_error";
    }
    if (!stats.header_sent) {
        return "no_response";
    }
    return stats.completed ? "ok" : "incomplete";
}

std::uint64_t elapsed_us(std::chrono::steady_clock::time_point begin,
                         std::chrono::steady_clock::time_point end) noexcept {
    if (end <= begin) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

} // namespace

std::uint64_t next_request_id() noexcept { return g_next_request_id.fetch_add(1, std::memory_order_relaxed); }

void write_access_log(const fiber::http::HttpExchange &exchange, const RequestLogContext &context,
                      std::chrono::steady_clock::time_point finished_at) noexcept {
    if (!context.access_log || !LOG_ACCESS.get().enabled(fiber::log::LogLevel::Info)) {
        return;
    }

    constexpr std::size_t kMaxMethodLength = 32;
    constexpr std::size_t kMaxPathLength = 2048;
    constexpr std::size_t kMaxHostLength = 512;
    constexpr std::size_t kMaxNameLength = 512;
    std::array<char, INET6_ADDRSTRLEN> remote_buffer{};
    const std::string_view remote = remote_ip(exchange.remote_addr().ip(), remote_buffer);
    const auto &stats = exchange.response_stats();
    const auto *host = exchange.host_header();
    const std::string_view host_value = host ? host->value_view().substr(0, kMaxHostLength) : std::string_view("-");
    const std::string_view path =
            exchange.uri().path.empty() ? std::string_view("/") : exchange.uri().path.substr(0, kMaxPathLength);
    const std::string_view server =
            context.server_name.empty() ? std::string_view("-") : context.server_name.substr(0, kMaxNameLength);
    const std::string_view location = context.location_pattern.empty()
                                              ? std::string_view("-")
                                              : context.location_pattern.substr(0, kMaxNameLength);
    const std::string_view upstream =
            context.upstream_host.empty() ? std::string_view("-") : context.upstream_host.substr(0, kMaxNameLength);
    const std::uint64_t upstream_time =
            context.upstream_started ? elapsed_us(context.upstream_started_at, finished_at) : 0;

    LOG(LOG_ACCESS, INFO) << "request_id=" << context.request_id << " remote_addr=" << fiber::log::quoted(remote)
                          << " remote_port=" << exchange.remote_addr().port()
                          << " method=" << fiber::log::quoted(exchange.method_view().substr(0, kMaxMethodLength))
                          << " path=" << fiber::log::quoted(path) << " protocol=" << protocol_name(exchange.version())
                          << " host=" << fiber::log::quoted(host_value) << " server=" << fiber::log::quoted(server)
                          << " location=" << fiber::log::quoted(location) << " status=" << stats.status_code
                          << " body_bytes_sent=" << stats.body_bytes_sent
                          << " request_time_us=" << elapsed_us(context.started_at, finished_at)
                          << " upstream=" << fiber::log::quoted(upstream) << " upstream_port=" << context.upstream_port
                          << " upstream_status=" << context.upstream_status << " upstream_time_us=" << upstream_time
                          << " outcome=" << access_outcome(stats, context);
}

} // namespace fiber::lite_nginx::logging
