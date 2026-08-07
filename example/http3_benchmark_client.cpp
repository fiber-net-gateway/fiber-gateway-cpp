#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <netdb.h>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/Timeout.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp3Exchange.h>
#include <fiber/http/ClientHttp3Types.h>
#include <fiber/http/Http3Client.h>
#include <fiber/http/Http3ClientConnection.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicProtocol.h>
#include <fiber/quic/QuicUdpEndpoint.h>

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kMaxConsecutiveNotSentFailures = 32;
constexpr auto kRequestFailureBackoff = 1ms;

volatile std::sig_atomic_t g_stop_requested = 0;

void on_signal(int) { g_stop_requested = 1; }

enum class LoadMode : std::uint8_t {
    Closed,
    Rate,
};

enum class RequestPhase : std::uint8_t {
    StreamOrHeaderSend,
    BodySend,
    HeaderRead,
    BodyRead,
    StatusValidation,
    LengthValidation,
    Count,
};

enum class SetupPhase : std::uint8_t {
    None,
    EndpointInit,
    EndpointStart,
    ClientInit,
    RequestBody,
    Connect,
};

constexpr std::size_t kRequestPhaseCount = static_cast<std::size_t>(RequestPhase::Count);
constexpr std::size_t kIoErrCount = static_cast<std::size_t>(fiber::common::IoErr::Unknown) + 1;
constexpr std::size_t kOutcomeCount = 4;
constexpr std::size_t kStatusClassCount = 6;
constexpr std::size_t kMaxLaneCount = 1024 * 1024;

struct Target {
    std::string url;
    std::string host;
    std::string authority;
    std::string path;
    fiber::net::SocketAddress remote;
    std::uint16_t port = 443;
};

struct BenchmarkOptions {
    Target target;
    std::string connect_to;
    std::string ca_file;
    std::string body_file;
    std::string body_size_text;
    std::string json_file;
    std::vector<std::uint8_t> body;
    LoadMode mode = LoadMode::Closed;
    fiber::http::HttpMethod method = fiber::http::HttpMethod::Get;
    std::chrono::milliseconds warmup{1000};
    std::chrono::milliseconds duration{10000};
    std::chrono::milliseconds drain{5000};
    std::chrono::milliseconds request_timeout{5000};
    std::chrono::milliseconds handshake_timeout{10000};
    std::size_t threads = 1;
    std::size_t connections = 1;
    std::size_t streams = 1;
    std::uint64_t rps = 0;
    std::optional<std::uint64_t> expected_bytes;
    int expected_status = 200;
    bool insecure = false;
    bool pacing_enabled = false;
};

[[nodiscard]] constexpr std::string_view mode_name(LoadMode mode) noexcept {
    return mode == LoadMode::Rate ? "rate" : "closed";
}

[[nodiscard]] constexpr std::string_view method_name(fiber::http::HttpMethod method) noexcept {
    return method == fiber::http::HttpMethod::Post ? "POST" : "GET";
}

[[nodiscard]] constexpr std::string_view request_phase_name(RequestPhase phase) noexcept {
    switch (phase) {
        case RequestPhase::StreamOrHeaderSend:
            return "stream_or_header_send";
        case RequestPhase::BodySend:
            return "body_send";
        case RequestPhase::HeaderRead:
            return "header_read";
        case RequestPhase::BodyRead:
            return "body_read";
        case RequestPhase::StatusValidation:
            return "status_validation";
        case RequestPhase::LengthValidation:
            return "length_validation";
        case RequestPhase::Count:
            break;
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view setup_phase_name(SetupPhase phase) noexcept {
    switch (phase) {
        case SetupPhase::None:
            return "none";
        case SetupPhase::EndpointInit:
            return "endpoint_init";
        case SetupPhase::EndpointStart:
            return "endpoint_start";
        case SetupPhase::ClientInit:
            return "client_init";
        case SetupPhase::RequestBody:
            return "request_body";
        case SetupPhase::Connect:
            return "connect";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view http3_connect_phase_name(fiber::http::Http3ClientConnectPhase phase) noexcept {
    switch (phase) {
        case fiber::http::Http3ClientConnectPhase::ClientInit:
            return "client_init";
        case fiber::http::Http3ClientConnectPhase::Quic:
            return "quic";
        case fiber::http::Http3ClientConnectPhase::Alpn:
            return "alpn";
        case fiber::http::Http3ClientConnectPhase::Http3:
            return "http3";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view quic_connect_phase_name(fiber::quic::QuicConnectPhase phase) noexcept {
    switch (phase) {
        case fiber::quic::QuicConnectPhase::Endpoint:
            return "endpoint";
        case fiber::quic::QuicConnectPhase::Connection:
            return "connection";
        case fiber::quic::QuicConnectPhase::InitialCrypto:
            return "initial_crypto";
        case fiber::quic::QuicConnectPhase::Tls:
            return "tls";
        case fiber::quic::QuicConnectPhase::Handshake:
            return "handshake";
        case fiber::quic::QuicConnectPhase::VersionNegotiation:
            return "version_negotiation";
        case fiber::quic::QuicConnectPhase::TransportParameters:
            return "transport_parameters";
        case fiber::quic::QuicConnectPhase::Timeout:
            return "timeout";
        case fiber::quic::QuicConnectPhase::PeerClose:
            return "peer_close";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view outcome_name(std::size_t index) noexcept {
    switch (static_cast<fiber::http::Http3RequestOutcome>(index)) {
        case fiber::http::Http3RequestOutcome::NotSent:
            return "not_sent";
        case fiber::http::Http3RequestOutcome::Rejected:
            return "rejected";
        case fiber::http::Http3RequestOutcome::PossiblyProcessed:
            return "possibly_processed";
        case fiber::http::Http3RequestOutcome::Complete:
            return "complete";
    }
    return "unknown";
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char *first = text.data();
    const char *last = first + text.size();
    auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::chrono::milliseconds> parse_duration(std::string_view text) noexcept {
    std::uint64_t multiplier = 0;
    if (text.ends_with("ms")) {
        multiplier = 1;
        text.remove_suffix(2);
    } else if (text.ends_with('s')) {
        multiplier = 1000;
        text.remove_suffix(1);
    } else if (text.ends_with('m')) {
        multiplier = 60 * 1000;
        text.remove_suffix(1);
    } else {
        return std::nullopt;
    }
    auto value = parse_unsigned(text);
    if (!value || *value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / multiplier) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(*value * multiplier);
}

[[nodiscard]] bool parse_port(std::string_view text, std::uint16_t &port) noexcept {
    auto value = parse_unsigned(text);
    if (!value || *value == 0 || *value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(*value);
    return true;
}

struct HostPort {
    std::string_view host;
    std::uint16_t port = 0;
};

[[nodiscard]] std::optional<HostPort> parse_host_port(std::string_view text, std::uint16_t default_port,
                                                      bool require_port) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    HostPort out{.port = default_port};
    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close == 1) {
            return std::nullopt;
        }
        out.host = text.substr(1, close - 1);
        const std::string_view suffix = text.substr(close + 1);
        if (suffix.empty()) {
            return require_port ? std::nullopt : std::optional<HostPort>(out);
        }
        if (suffix.front() != ':' || !parse_port(suffix.substr(1), out.port)) {
            return std::nullopt;
        }
        return out;
    }

    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        if (require_port) {
            return std::nullopt;
        }
        out.host = text;
        return out.host.empty() ? std::nullopt : std::optional<HostPort>(out);
    }
    if (text.find(':') != colon) {
        return std::nullopt;
    }
    out.host = text.substr(0, colon);
    if (out.host.empty() || !parse_port(text.substr(colon + 1), out.port)) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] bool parse_url(std::string_view url, Target &target, std::string &error) {
    constexpr std::string_view kScheme = "https://";
    if (!url.starts_with(kScheme)) {
        error = "target URL must start with https://";
        return false;
    }
    if (url.find('#') != std::string_view::npos) {
        error = "URL fragments are not supported";
        return false;
    }
    std::string_view rest = url.substr(kScheme.size());
    const std::size_t slash = rest.find('/');
    const std::string_view authority = rest.substr(0, slash);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        error = "URL authority is invalid";
        return false;
    }
    auto host_port = parse_host_port(authority, 443, false);
    if (!host_port) {
        error = "URL host or port is invalid";
        return false;
    }
    target.url.assign(url);
    target.host.assign(host_port->host);
    target.authority.assign(authority);
    target.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    target.port = host_port->port;
    return true;
}

[[nodiscard]] bool parse_literal_address(std::string_view text, fiber::net::SocketAddress &out, std::string &error) {
    auto host_port = parse_host_port(text, 0, true);
    if (!host_port) {
        error = "--connect-to must be IP:PORT or [IPv6]:PORT";
        return false;
    }
    fiber::net::IpAddress ip;
    if (!fiber::net::IpAddress::parse(host_port->host, ip)) {
        error = "--connect-to requires a numeric IP address";
        return false;
    }
    out = fiber::net::SocketAddress(ip, host_port->port);
    return true;
}

[[nodiscard]] bool resolve_target(BenchmarkOptions &options, std::string &error) {
    if (!options.connect_to.empty()) {
        return parse_literal_address(options.connect_to, options.target.remote, error);
    }

    fiber::net::IpAddress literal;
    if (fiber::net::IpAddress::parse(options.target.host, literal)) {
        options.target.remote = fiber::net::SocketAddress(literal, options.target.port);
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo *addresses = nullptr;
    const std::string service = std::to_string(options.target.port);
    const int resolved = ::getaddrinfo(options.target.host.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0) {
        error = std::string("DNS resolution failed: ") + ::gai_strerror(resolved);
        return false;
    }

    bool found = false;
    for (addrinfo *entry = addresses; entry != nullptr; entry = entry->ai_next) {
        if (fiber::net::SocketAddress::from_sockaddr(entry->ai_addr, entry->ai_addrlen, options.target.remote)) {
            found = true;
            break;
        }
    }
    ::freeaddrinfo(addresses);
    if (!found) {
        error = "DNS result contained no usable UDP address";
    }
    return found;
}

void print_usage(std::ostream &out) {
    out << "usage: http3_benchmark_client <https-url> [options]\n"
           "\n"
           "load options:\n"
           "  --mode closed|rate       load model (default: closed)\n"
           "  --rps N                  aggregate target RPS for rate mode\n"
           "  --threads N              event-loop threads (default: 1)\n"
           "  --connections N          total QUIC connections (default: 1)\n"
           "  --streams N              request lanes per connection (default: 1)\n"
           "  --warmup D               warmup duration, e.g. 1s or 500ms\n"
           "  --duration D             measurement duration (default: 10s)\n"
           "  --drain D                maximum drain duration (default: 5s)\n"
           "  --timeout D              absolute request timeout (default: 5s)\n"
           "  --handshake-timeout D    QUIC handshake timeout (default: 10s)\n"
           "\n"
           "request options:\n"
           "  --method GET|POST         request method (default: GET)\n"
           "  --body FILE               POST body file\n"
           "  --expect-status N         expected final status (default: 200)\n"
           "  --expect-bytes N          expected response body length\n"
           "\n"
           "connection options:\n"
           "  --connect-to IP:PORT      bypass DNS while retaining URL SNI/authority\n"
           "  --ca-file FILE            trusted CA bundle or certificate\n"
           "  --insecure                disable peer certificate verification\n"
           "  --pacing on|off           QUIC send pacing (default: off)\n"
           "\n"
           "output options:\n"
           "  --json FILE               also write a JSON summary\n"
           "  --help                    show this help\n";
}

[[nodiscard]] bool read_body_file(BenchmarkOptions &options, std::string &error) {
    if (options.body_file.empty()) {
        options.body_size_text = "0";
        return true;
    }
    std::ifstream input(options.body_file, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "failed to open request body: " + options.body_file;
        return false;
    }
    const std::streamoff end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max()) {
        error = "request body is too large";
        return false;
    }
    options.body.resize(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!options.body.empty()) {
        input.read(reinterpret_cast<char *>(options.body.data()), static_cast<std::streamsize>(options.body.size()));
        if (!input) {
            error = "failed to read request body: " + options.body_file;
            return false;
        }
    }
    options.body_size_text = std::to_string(options.body.size());
    return true;
}

[[nodiscard]] bool parse_options(int argc, char **argv, BenchmarkOptions &options, std::string &error,
                                 bool &show_help) {
    std::string_view url;
    auto next_value = [&](int &index, std::string_view name) -> std::optional<std::string_view> {
        if (index + 1 >= argc) {
            error = std::string(name) + " requires a value";
            return std::nullopt;
        }
        return std::string_view(argv[++index]);
    };
    auto parse_count = [&](std::string_view text, std::size_t &out, std::string_view name) {
        auto value = parse_unsigned(text);
        if (!value || *value == 0 || *value > std::numeric_limits<std::size_t>::max()) {
            error = std::string("invalid ") + std::string(name);
            return false;
        }
        out = static_cast<std::size_t>(*value);
        return true;
    };
    auto parse_time = [&](std::string_view text, std::chrono::milliseconds &out, std::string_view name,
                          bool allow_zero) {
        auto value = parse_duration(text);
        if (!value || (!allow_zero && value->count() == 0)) {
            error = std::string("invalid ") + std::string(name);
            return false;
        }
        out = *value;
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            show_help = true;
            return true;
        }
        if (!arg.starts_with('-')) {
            if (!url.empty()) {
                error = "only one target URL is allowed";
                return false;
            }
            url = arg;
            continue;
        }
        auto take = [&](std::string_view name) { return next_value(i, name); };
        if (arg == "--insecure") {
            options.insecure = true;
        } else if (arg == "--mode") {
            auto value = take(arg);
            if (!value) {
                return false;
            }
            if (*value == "closed") {
                options.mode = LoadMode::Closed;
            } else if (*value == "rate") {
                options.mode = LoadMode::Rate;
            } else {
                error = "--mode must be closed or rate";
                return false;
            }
        } else if (arg == "--rps") {
            auto value = take(arg);
            auto parsed = value ? parse_unsigned(*value) : std::nullopt;
            if (!parsed || *parsed == 0) {
                error = "invalid --rps";
                return false;
            }
            options.rps = *parsed;
        } else if (arg == "--threads") {
            auto value = take(arg);
            if (!value || !parse_count(*value, options.threads, arg)) {
                return false;
            }
        } else if (arg == "--connections") {
            auto value = take(arg);
            if (!value || !parse_count(*value, options.connections, arg)) {
                return false;
            }
        } else if (arg == "--streams") {
            auto value = take(arg);
            if (!value || !parse_count(*value, options.streams, arg)) {
                return false;
            }
        } else if (arg == "--warmup") {
            auto value = take(arg);
            if (!value || !parse_time(*value, options.warmup, arg, true)) {
                return false;
            }
        } else if (arg == "--duration") {
            auto value = take(arg);
            if (!value || !parse_time(*value, options.duration, arg, false)) {
                return false;
            }
        } else if (arg == "--drain") {
            auto value = take(arg);
            if (!value || !parse_time(*value, options.drain, arg, true)) {
                return false;
            }
        } else if (arg == "--timeout") {
            auto value = take(arg);
            if (!value || !parse_time(*value, options.request_timeout, arg, false)) {
                return false;
            }
        } else if (arg == "--handshake-timeout") {
            auto value = take(arg);
            if (!value || !parse_time(*value, options.handshake_timeout, arg, false)) {
                return false;
            }
        } else if (arg == "--method") {
            auto value = take(arg);
            if (!value) {
                return false;
            }
            if (*value == "GET" || *value == "get") {
                options.method = fiber::http::HttpMethod::Get;
            } else if (*value == "POST" || *value == "post") {
                options.method = fiber::http::HttpMethod::Post;
            } else {
                error = "--method must be GET or POST";
                return false;
            }
        } else if (arg == "--body") {
            auto value = take(arg);
            if (!value) {
                return false;
            }
            options.body_file.assign(*value);
        } else if (arg == "--expect-status") {
            auto value = take(arg);
            auto parsed = value ? parse_unsigned(*value) : std::nullopt;
            if (!parsed || *parsed < 100 || *parsed > 999) {
                error = "invalid --expect-status";
                return false;
            }
            options.expected_status = static_cast<int>(*parsed);
        } else if (arg == "--expect-bytes") {
            auto value = take(arg);
            auto parsed = value ? parse_unsigned(*value) : std::nullopt;
            if (!parsed) {
                error = "invalid --expect-bytes";
                return false;
            }
            options.expected_bytes = *parsed;
        } else if (arg == "--connect-to") {
            auto value = take(arg);
            if (!value) {
                return false;
            }
            options.connect_to.assign(*value);
        } else if (arg == "--ca-file") {
            auto value = take(arg);
            if (!value) {
                return false;
            }
            options.ca_file.assign(*value);
        } else if (arg == "--pacing") {
            auto value = take(arg);
            if (!value) {
                return false;
            }
            if (*value == "on") {
                options.pacing_enabled = true;
            } else if (*value == "off") {
                options.pacing_enabled = false;
            } else {
                error = "--pacing must be on or off";
                return false;
            }
        } else if (arg == "--json") {
            auto value = take(arg);
            if (!value) {
                return false;
            }
            options.json_file.assign(*value);
        } else {
            error = "unknown option: " + std::string(arg);
            return false;
        }
    }

    if (url.empty()) {
        error = "missing target URL";
        return false;
    }
    if (!parse_url(url, options.target, error)) {
        return false;
    }
    if (options.threads > options.connections) {
        error = "--threads cannot exceed --connections";
        return false;
    }
    if (options.connections > std::numeric_limits<std::size_t>::max() / options.streams ||
        options.connections * options.streams > kMaxLaneCount) {
        error = "connections × streams exceeds the lane safety limit";
        return false;
    }
    if (options.mode == LoadMode::Rate && options.rps == 0) {
        error = "--mode rate requires --rps";
        return false;
    }
    if (options.mode == LoadMode::Closed && options.rps != 0) {
        error = "--rps is only valid with --mode rate";
        return false;
    }
    if (!options.body_file.empty() && options.method != fiber::http::HttpMethod::Post) {
        error = "--body requires --method POST";
        return false;
    }
    if (!read_body_file(options, error)) {
        return false;
    }
    return resolve_target(options, error);
}

class LatencyHistogram {
public:
    void record(std::chrono::steady_clock::duration duration) noexcept {
        std::uint64_t us = 0;
        if (duration > std::chrono::steady_clock::duration::zero()) {
            us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
        }
        const std::size_t index = index_for(us);
        ++buckets_[index];
        ++count_;
        max_us_ = std::max(max_us_, us);
    }

    void merge(const LatencyHistogram &other) noexcept {
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            buckets_[i] += other.buckets_[i];
        }
        count_ += other.count_;
        max_us_ = std::max(max_us_, other.max_us_);
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t max_us() const noexcept { return max_us_; }

    [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const std::uint64_t rank = std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(fraction * static_cast<double>(count_) + 0.999999));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            seen += buckets_[i];
            if (seen >= rank) {
                return lower_bound_for(i);
            }
        }
        return max_us_;
    }

private:
    static constexpr std::size_t kLinearBuckets = 256;
    static constexpr std::size_t kLogGroups = 56;
    static constexpr std::size_t kBucketCount = kLinearBuckets + kLogGroups * kLinearBuckets;

    [[nodiscard]] static std::size_t index_for(std::uint64_t value) noexcept {
        if (value < kLinearBuckets) {
            return static_cast<std::size_t>(value);
        }
        const unsigned shift = std::bit_width(value) - 9;
        const std::uint64_t mantissa = value >> shift;
        const std::size_t index = kLinearBuckets + static_cast<std::size_t>(shift) * kLinearBuckets +
                                  static_cast<std::size_t>(mantissa - kLinearBuckets);
        return std::min(index, kBucketCount - 1);
    }

    [[nodiscard]] static std::uint64_t lower_bound_for(std::size_t index) noexcept {
        if (index < kLinearBuckets) {
            return index;
        }
        const std::size_t relative = index - kLinearBuckets;
        const unsigned shift = static_cast<unsigned>(relative / kLinearBuckets);
        const std::uint64_t mantissa = kLinearBuckets + relative % kLinearBuckets;
        return mantissa << shift;
    }

    std::array<std::uint64_t, kBucketCount> buckets_{};
    std::uint64_t count_ = 0;
    std::uint64_t max_us_ = 0;
};

struct WorkerStats {
    std::uint64_t started = 0;
    std::uint64_t finished = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t response_bytes = 0;
    std::uint64_t warmup_started = 0;
    std::uint64_t warmup_errors = 0;
    std::uint64_t terminal_lane_stops = 0;
    std::uint64_t failure_guard_lane_stops = 0;
    std::uint64_t endpoint_dropped_datagrams = 0;
    std::uint64_t endpoint_recv_storage_high_water = 0;
    std::uint64_t endpoint_recv_storage_rejected = 0;
    std::uint64_t path_sent_bytes = 0;
    std::uint64_t path_received_bytes = 0;
    std::uint64_t app_packets_started = 0;
    std::uint64_t key_updates = 0;
    std::uint64_t current_key_packets = 0;
    std::uint64_t cwnd_bytes = 0;
    std::uint64_t in_flight_bytes = 0;
    std::uint64_t pto_count = 0;
    std::uint64_t rtt_samples = 0;
    std::uint64_t rtt_sum_ms = 0;
    std::array<std::uint64_t, kRequestPhaseCount> phase_errors{};
    std::array<std::uint64_t, kIoErrCount> io_errors{};
    std::array<std::uint64_t, kRequestPhaseCount> warmup_phase_errors{};
    std::array<std::uint64_t, kIoErrCount> warmup_io_errors{};
    std::array<std::uint64_t, kOutcomeCount> outcomes{};
    std::array<std::uint64_t, kStatusClassCount> statuses{};
    LatencyHistogram ttfb;
    LatencyHistogram total;
    LatencyHistogram queue_delay;
    LatencyHistogram corrected;

    void merge(const WorkerStats &other) noexcept {
        started += other.started;
        finished += other.finished;
        succeeded += other.succeeded;
        failed += other.failed;
        response_bytes += other.response_bytes;
        warmup_started += other.warmup_started;
        warmup_errors += other.warmup_errors;
        terminal_lane_stops += other.terminal_lane_stops;
        failure_guard_lane_stops += other.failure_guard_lane_stops;
        endpoint_dropped_datagrams += other.endpoint_dropped_datagrams;
        endpoint_recv_storage_high_water += other.endpoint_recv_storage_high_water;
        endpoint_recv_storage_rejected += other.endpoint_recv_storage_rejected;
        path_sent_bytes += other.path_sent_bytes;
        path_received_bytes += other.path_received_bytes;
        app_packets_started += other.app_packets_started;
        key_updates += other.key_updates;
        current_key_packets += other.current_key_packets;
        cwnd_bytes += other.cwnd_bytes;
        in_flight_bytes += other.in_flight_bytes;
        pto_count += other.pto_count;
        rtt_samples += other.rtt_samples;
        rtt_sum_ms += other.rtt_sum_ms;
        for (std::size_t i = 0; i < phase_errors.size(); ++i) {
            phase_errors[i] += other.phase_errors[i];
        }
        for (std::size_t i = 0; i < io_errors.size(); ++i) {
            io_errors[i] += other.io_errors[i];
        }
        for (std::size_t i = 0; i < warmup_phase_errors.size(); ++i) {
            warmup_phase_errors[i] += other.warmup_phase_errors[i];
        }
        for (std::size_t i = 0; i < warmup_io_errors.size(); ++i) {
            warmup_io_errors[i] += other.warmup_io_errors[i];
        }
        for (std::size_t i = 0; i < outcomes.size(); ++i) {
            outcomes[i] += other.outcomes[i];
        }
        for (std::size_t i = 0; i < statuses.size(); ++i) {
            statuses[i] += other.statuses[i];
        }
        ttfb.merge(other.ttfb);
        total.merge(other.total);
        queue_delay.merge(other.queue_delay);
        corrected.merge(other.corrected);
    }
};

struct RunCoordinator {
    std::atomic<std::size_t> setup_done{0};
    std::atomic<std::size_t> workers_done{0};
    std::atomic<std::int64_t> warmup_start_ns{0};
    std::atomic<std::int64_t> measurement_stop_ns{0};
    std::atomic<bool> setup_failed{false};
    std::atomic<bool> stop{false};
    std::size_t worker_count = 0;
};

[[nodiscard]] std::int64_t time_point_ns(std::chrono::steady_clock::time_point time) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::steady_clock::time_point time_point_from_ns(std::int64_t value) noexcept {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(value));
}

[[nodiscard]] std::chrono::milliseconds remaining_timeout(std::chrono::steady_clock::time_point deadline,
                                                          std::chrono::steady_clock::time_point now) noexcept {
    if (now >= deadline) {
        return 0ms;
    }
    return std::max(1ms, std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
}

struct RequestResult {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    RequestPhase error_phase = RequestPhase::Count;
    fiber::http::Http3RequestOutcome outcome = fiber::http::Http3RequestOutcome::NotSent;
    std::chrono::steady_clock::duration ttfb{};
    std::chrono::steady_clock::duration total{};
    std::chrono::steady_clock::time_point started_at{};
    std::uint64_t body_bytes = 0;
    int status = 0;
    bool final_header_received = false;
    bool response_complete = false;
    bool status_valid = false;
    bool length_valid = false;

    [[nodiscard]] bool succeeded() const noexcept {
        return error == fiber::common::IoErr::None && response_complete && status_valid && length_valid &&
               outcome == fiber::http::Http3RequestOutcome::Complete;
    }
};

class BenchmarkWorker {
public:
    BenchmarkWorker(std::size_t index, std::size_t connection_count, fiber::event::EventLoop &loop,
                    const BenchmarkOptions &options, RunCoordinator &coordinator) noexcept :
        index_(index), connection_count_(connection_count), loop_(loop), options_(options), coordinator_(coordinator) {}

    [[nodiscard]] fiber::async::DetachedTask run() {
        if (!co_await setup()) {
            coordinator_.setup_failed.store(true, std::memory_order_release);
            coordinator_.setup_done.fetch_add(1, std::memory_order_release);
            cleanup_immediate();
            coordinator_.workers_done.fetch_add(1, std::memory_order_release);
            co_return;
        }

        coordinator_.setup_done.fetch_add(1, std::memory_order_release);
        while (coordinator_.warmup_start_ns.load(std::memory_order_acquire) == 0 &&
               !coordinator_.stop.load(std::memory_order_acquire)) {
            co_await fiber::async::sleep(1ms);
        }
        if (coordinator_.stop.load(std::memory_order_acquire)) {
            co_await cleanup();
            coordinator_.workers_done.fetch_add(1, std::memory_order_release);
            co_return;
        }

        warmup_start_ = time_point_from_ns(coordinator_.warmup_start_ns.load(std::memory_order_acquire));
        measurement_start_ = warmup_start_ + options_.warmup;
        measurement_end_ = measurement_start_ + options_.duration;
        drain_end_ = measurement_end_ + options_.drain;

        lane_group_.add(connection_count_ * options_.streams);
        for (std::size_t connection_index = 0; connection_index < connection_count_; ++connection_index) {
            for (std::size_t lane = 0; lane < options_.streams; ++lane) {
                fiber::async::spawn([this, connection_index]() { return run_lane(connection_index); });
            }
        }
        stop_monitor_group_.add(1);
        fiber::async::spawn([this]() { return monitor_stop(); });
        co_await lane_group_.join();
        lanes_done_ = true;
        co_await stop_monitor_group_.join();

        collect_transport_stats();
        co_await cleanup();
        coordinator_.workers_done.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] const WorkerStats &stats() const noexcept { return stats_; }
    [[nodiscard]] SetupPhase setup_phase() const noexcept { return setup_phase_; }
    [[nodiscard]] fiber::common::IoErr setup_error() const noexcept { return setup_error_; }
    [[nodiscard]] const fiber::http::Http3ClientConnectError &connect_error() const noexcept { return connect_error_; }
    [[nodiscard]] std::string local_address() const {
        return local_address_.empty() ? std::string("unavailable") : local_address_;
    }

private:
    [[nodiscard]] bool fail_setup(SetupPhase phase, fiber::common::IoErr error) noexcept {
        setup_phase_ = phase;
        setup_error_ = error;
        return false;
    }

    [[nodiscard]] fiber::async::Task<bool> setup() {
        fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
        endpoint_options.bind_addr = options_.target.remote.ip().is_v4() ? fiber::net::SocketAddress::any_v4(0)
                                                                         : fiber::net::SocketAddress::any_v6(0);
        endpoint_options.max_connections = connection_count_ + 4;
        endpoint_options.send.pacing.enabled = options_.pacing_enabled;
        auto endpoint_initialized = endpoint_.init(loop_, endpoint_options);
        if (!endpoint_initialized) {
            co_return fail_setup(SetupPhase::EndpointInit, endpoint_initialized.error());
        }
        auto endpoint_started = endpoint_.start();
        if (!endpoint_started) {
            co_return fail_setup(SetupPhase::EndpointStart, endpoint_started.error());
        }
        local_address_ = endpoint_.local_addr().to_string();

        fiber::http::Http3Client::Options client_options{};
        client_options.tls.ca_file = options_.ca_file;
        client_options.verify_peer = !options_.insecure;
        client_options.drain_timeout = options_.drain;
        client_ = std::make_unique<fiber::http::Http3Client>(endpoint_, std::move(client_options));
        auto client_initialized = client_->init();
        if (!client_initialized) {
            co_return fail_setup(SetupPhase::ClientInit, client_initialized.error());
        }

        if (!options_.body.empty()) {
            request_body_ = fiber::mem::IoBuf::allocate(options_.body.size());
            if (!request_body_) {
                co_return fail_setup(SetupPhase::RequestBody, fiber::common::IoErr::NoMem);
            }
            std::memcpy(request_body_.writable_data(), options_.body.data(), options_.body.size());
            request_body_.commit(options_.body.size());
        }

        connections_.reserve(connection_count_);
        for (std::size_t i = 0; i < connection_count_; ++i) {
            fiber::quic::QuicClientConnectOptions connect_options{};
            connect_options.remote_addr = options_.target.remote;
            connect_options.server_name = options_.target.host;
            connect_options.handshake_timeout = options_.handshake_timeout;
            connect_options.allow_insecure = options_.insecure;
            auto connected = co_await client_->connect(std::move(connect_options));
            if (!connected) {
                connect_error_ = connected.error();
                co_return fail_setup(SetupPhase::Connect, connected.error().io_error);
            }
            connections_.push_back(std::move(*connected));
        }
        co_return true;
    }

    [[nodiscard]] fiber::async::DetachedTask run_lane(std::size_t connection_index) {
        struct DoneGuard {
            fiber::async::WaitGroup *group = nullptr;
            ~DoneGuard() { group->done(); }
        } done{&lane_group_};

        std::uint32_t consecutive_not_sent_failures = 0;
        fiber::http::Http3ClientConnection &connection = connections_[connection_index];
        co_await sleep_until(warmup_start_);
        while (!stop_requested()) {
            auto now = fiber::event::EventLoop::current().now();
            std::chrono::steady_clock::time_point scheduled = now;
            bool measured = false;

            if (options_.mode == LoadMode::Closed) {
                if (now >= measurement_end_) {
                    break;
                }
                measured = now >= measurement_start_;
            } else {
                const std::uint64_t ticket = next_rate_ticket_++;
                const std::uint64_t global_ticket =
                        index_ + ticket * static_cast<std::uint64_t>(coordinator_.worker_count);
                scheduled = scheduled_time(global_ticket);
                if (scheduled >= measurement_end_) {
                    break;
                }
                now = fiber::event::EventLoop::current().now();
                if (scheduled < measurement_start_ && now >= measurement_start_) {
                    continue;
                }
                co_await sleep_until(scheduled);
                now = fiber::event::EventLoop::current().now();
                if (stop_requested() || now >= measurement_end_) {
                    break;
                }
                measured = scheduled >= measurement_start_;
            }

            if (!connection.http3().accepting_requests()) {
                ++stats_.terminal_lane_stops;
                break;
            }
            RequestResult result = co_await run_request(connection);
            record_result(result, measured, scheduled);
            if (!connection.http3().accepting_requests()) {
                ++stats_.terminal_lane_stops;
                break;
            }
            if (result.succeeded()) {
                consecutive_not_sent_failures = 0;
                continue;
            }

            if (result.error_phase == RequestPhase::StreamOrHeaderSend &&
                result.outcome == fiber::http::Http3RequestOutcome::NotSent) {
                ++consecutive_not_sent_failures;
                if (consecutive_not_sent_failures >= kMaxConsecutiveNotSentFailures) {
                    ++stats_.failure_guard_lane_stops;
                    break;
                }
            } else {
                consecutive_not_sent_failures = 0;
            }
            co_await fiber::async::sleep(kRequestFailureBackoff);
        }
    }

    [[nodiscard]] fiber::async::DetachedTask monitor_stop() noexcept {
        struct DoneGuard {
            fiber::async::WaitGroup *group = nullptr;
            ~DoneGuard() { group->done(); }
        } done{&stop_monitor_group_};

        while (!lanes_done_ && !stop_requested()) {
            co_await fiber::async::sleep(10ms);
        }
        if (lanes_done_) {
            co_return;
        }

        const std::int64_t stopped_at_ns = coordinator_.measurement_stop_ns.load(std::memory_order_acquire);
        const auto stopped_at =
                stopped_at_ns == 0 ? fiber::event::EventLoop::current().now() : time_point_from_ns(stopped_at_ns);
        const auto force_close_at = stopped_at + options_.drain;
        while (!lanes_done_) {
            const auto now = fiber::event::EventLoop::current().now();
            if (now >= force_close_at) {
                break;
            }
            co_await fiber::async::sleep(std::min(force_close_at - now, std::chrono::steady_clock::duration(10ms)));
        }
        if (lanes_done_) {
            co_return;
        }
        for (auto &connection: connections_) {
            connection.shutdown(fiber::http::Http3ErrorCode::RequestCancelled);
        }
    }

    [[nodiscard]] fiber::async::Task<RequestResult>
    run_request(fiber::http::Http3ClientConnection &connection) noexcept {
        RequestResult result{};
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        const bool has_request_body = options_.method == fiber::http::HttpMethod::Post;
        if (has_request_body && headers.add_view("content-length", options_.body_size_text) == nullptr) {
            result.error = fiber::common::IoErr::NoMem;
            result.error_phase = RequestPhase::StreamOrHeaderSend;
            co_return result;
        }

        fiber::http::ClientHttp3Exchange exchange = connection.open_exchange(pool);
        const auto started_at = fiber::event::EventLoop::current().now();
        result.started_at = started_at;
        const auto request_deadline = std::min(started_at + options_.request_timeout, drain_end_);
        fiber::http::Http3RequestHead head{
                .method = options_.method,
                .scheme = "https",
                .authority = options_.target.authority,
                .path = options_.target.path,
                .headers = has_request_body ? &headers : nullptr,
        };

        auto sent_head = co_await exchange.send_request_header(
                head, !has_request_body, remaining_timeout(request_deadline, fiber::event::EventLoop::current().now()));
        if (!sent_head) {
            result.error = sent_head.error();
            result.error_phase = RequestPhase::StreamOrHeaderSend;
            result.outcome = exchange.outcome();
            (void) exchange.abort(result.error);
            co_return result;
        }

        if (has_request_body) {
            fiber::mem::IoBufChain body(fiber::event::EventLoop::current().io_buf_node_pool());
            body.mark_complete();
            if (request_body_) {
                fiber::mem::IoBuf slice = request_body_.retain_slice(0, request_body_.readable());
                if (!slice || !body.append(std::move(slice))) {
                    result.error = fiber::common::IoErr::NoMem;
                    result.error_phase = RequestPhase::BodySend;
                    result.outcome = exchange.outcome();
                    (void) exchange.abort(result.error);
                    co_return result;
                }
            }
            auto sent_body = co_await exchange.write_all(
                    std::move(body), remaining_timeout(request_deadline, fiber::event::EventLoop::current().now()));
            if (!sent_body) {
                result.error = sent_body.error();
                result.error_phase = RequestPhase::BodySend;
                result.outcome = exchange.outcome();
                (void) exchange.abort(result.error);
                co_return result;
            }
        }

        for (;;) {
            auto response_head = co_await exchange.read_header(
                    remaining_timeout(request_deadline, fiber::event::EventLoop::current().now()));
            if (!response_head) {
                result.error = response_head.error();
                result.error_phase = RequestPhase::HeaderRead;
                result.outcome = exchange.outcome();
                (void) exchange.abort(result.error);
                co_return result;
            }
            if (*response_head == nullptr) {
                result.error = fiber::common::IoErr::Invalid;
                result.error_phase = RequestPhase::HeaderRead;
                result.outcome = exchange.outcome();
                (void) exchange.abort(result.error);
                co_return result;
            }
            if ((*response_head)->kind == fiber::http::OutgoingHeaderKind::Informational) {
                continue;
            }
            result.status = (*response_head)->status_code;
            result.final_header_received = true;
            result.ttfb = fiber::event::EventLoop::current().now() - started_at;
            break;
        }

        while (!result.response_complete) {
            auto chunk = co_await exchange.read_body(
                    64 * 1024, remaining_timeout(request_deadline, fiber::event::EventLoop::current().now()));
            if (!chunk) {
                result.error = chunk.error();
                result.error_phase = RequestPhase::BodyRead;
                result.outcome = exchange.outcome();
                (void) exchange.abort(result.error);
                co_return result;
            }
            result.body_bytes += chunk->readable_bytes();
            if (chunk->complete() && exchange.outcome() == fiber::http::Http3RequestOutcome::Complete) {
                result.response_complete = true;
            }
        }

        result.total = fiber::event::EventLoop::current().now() - started_at;
        result.outcome = exchange.outcome();
        result.status_valid = result.status == options_.expected_status;
        result.length_valid = !options_.expected_bytes || result.body_bytes == *options_.expected_bytes;
        if (!result.status_valid) {
            result.error = fiber::common::IoErr::Invalid;
            result.error_phase = RequestPhase::StatusValidation;
        } else if (!result.length_valid) {
            result.error = fiber::common::IoErr::Invalid;
            result.error_phase = RequestPhase::LengthValidation;
        }
        co_return result;
    }

    void record_result(const RequestResult &result, bool measured,
                       std::chrono::steady_clock::time_point scheduled) noexcept {
        if (!measured) {
            ++stats_.warmup_started;
            if (!result.succeeded()) {
                ++stats_.warmup_errors;
                if (result.error_phase != RequestPhase::Count) {
                    ++stats_.warmup_phase_errors[static_cast<std::size_t>(result.error_phase)];
                }
                const std::size_t error_index = static_cast<std::size_t>(result.error);
                if (error_index < stats_.warmup_io_errors.size()) {
                    ++stats_.warmup_io_errors[error_index];
                }
            }
            return;
        }

        ++stats_.started;
        ++stats_.finished;
        stats_.response_bytes += result.body_bytes;
        const std::size_t outcome_index = static_cast<std::size_t>(result.outcome);
        if (outcome_index < stats_.outcomes.size()) {
            ++stats_.outcomes[outcome_index];
        }
        if (result.status >= 100 && result.status < 600) {
            ++stats_.statuses[static_cast<std::size_t>(result.status / 100)];
        } else {
            ++stats_.statuses[0];
        }

        if (!result.succeeded()) {
            ++stats_.failed;
            if (result.error_phase != RequestPhase::Count) {
                ++stats_.phase_errors[static_cast<std::size_t>(result.error_phase)];
            }
            const std::size_t error_index = static_cast<std::size_t>(result.error);
            if (error_index < stats_.io_errors.size()) {
                ++stats_.io_errors[error_index];
            }
            return;
        }

        ++stats_.succeeded;
        stats_.ttfb.record(result.ttfb);
        stats_.total.record(result.total);
        if (options_.mode == LoadMode::Rate) {
            const auto delay = result.started_at > scheduled ? result.started_at - scheduled
                                                             : std::chrono::steady_clock::duration::zero();
            stats_.queue_delay.record(delay);
            stats_.corrected.record(result.started_at + result.total - scheduled);
        }
    }

    [[nodiscard]] std::chrono::steady_clock::time_point scheduled_time(std::uint64_t ticket) const noexcept {
        const unsigned __int128 numerator = static_cast<unsigned __int128>(ticket) * 1000000000ULL;
        const auto offset = std::chrono::nanoseconds(static_cast<std::uint64_t>(numerator / options_.rps));
        return warmup_start_ + offset;
    }

    [[nodiscard]] fiber::async::Task<void> sleep_until(std::chrono::steady_clock::time_point deadline) noexcept {
        for (;;) {
            const auto now = fiber::event::EventLoop::current().now();
            if (now >= deadline || stop_requested()) {
                co_return;
            }
            co_await fiber::async::sleep(std::min(deadline - now, std::chrono::steady_clock::duration(100ms)));
        }
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return g_stop_requested != 0 || coordinator_.stop.load(std::memory_order_acquire);
    }

    void collect_transport_stats() noexcept {
        stats_.endpoint_dropped_datagrams = endpoint_.dropped_datagram_count();
        stats_.endpoint_recv_storage_high_water = endpoint_.retained_recv_storage_high_water();
        stats_.endpoint_recv_storage_rejected = endpoint_.retained_recv_storage_rejected_count();
        for (auto &connection: connections_) {
            fiber::quic::QuicConnection &quic = connection.quic();
            if (const fiber::quic::QuicPath *path = quic.active_path()) {
                stats_.path_sent_bytes += path->sent;
                stats_.path_received_bytes += path->received;
            }
            const auto &application = quic.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
            stats_.app_packets_started += application.next_packet_number;
            stats_.key_updates += quic.crypto().epoch().generation;
            stats_.current_key_packets += quic.crypto().epoch().encrypted_packets;
            stats_.cwnd_bytes += quic.congestion().window;
            stats_.in_flight_bytes += quic.congestion().in_flight;
            stats_.pto_count += quic.pto_count();
            if (quic.rtt().avg_rtt.count() >= 0) {
                ++stats_.rtt_samples;
                stats_.rtt_sum_ms += static_cast<std::uint64_t>(quic.rtt().avg_rtt.count());
            }
        }
    }

    [[nodiscard]] fiber::async::Task<void> cleanup() noexcept {
        for (auto &connection: connections_) {
            connection.graceful_shutdown(fiber::http::Http3ErrorCode::NoError);
        }
        close_group_.add(connections_.size());
        for (std::size_t i = 0; i < connections_.size(); ++i) {
            fiber::async::spawn([this, i]() -> fiber::async::DetachedTask {
                co_await connections_[i].wait_closed();
                close_group_.done();
            });
        }
        auto graceful = co_await fiber::async::timeout_for([this]() { return close_group_.join(); },
                                                           std::max(options_.drain, 1ms));
        if (!graceful) {
            for (auto &connection: connections_) {
                connection.shutdown(fiber::http::Http3ErrorCode::NoError);
            }
            co_await close_group_.join();
        }
        connections_.clear();
        endpoint_.close();
        client_.reset();
        request_body_ = fiber::mem::IoBuf{};
    }

    void cleanup_immediate() noexcept {
        for (auto &connection: connections_) {
            connection.shutdown(fiber::http::Http3ErrorCode::RequestCancelled);
        }
        connections_.clear();
        endpoint_.close();
        client_.reset();
        request_body_ = fiber::mem::IoBuf{};
    }

    std::size_t index_ = 0;
    std::size_t connection_count_ = 0;
    fiber::event::EventLoop &loop_;
    const BenchmarkOptions &options_;
    RunCoordinator &coordinator_;
    fiber::quic::QuicUdpEndpoint endpoint_;
    std::unique_ptr<fiber::http::Http3Client> client_;
    std::vector<fiber::http::Http3ClientConnection> connections_;
    fiber::mem::IoBuf request_body_;
    fiber::async::WaitGroup lane_group_;
    fiber::async::WaitGroup stop_monitor_group_;
    fiber::async::WaitGroup close_group_;
    WorkerStats stats_;
    std::string local_address_;
    std::uint64_t next_rate_ticket_ = 0;
    std::chrono::steady_clock::time_point warmup_start_{};
    std::chrono::steady_clock::time_point measurement_start_{};
    std::chrono::steady_clock::time_point measurement_end_{};
    std::chrono::steady_clock::time_point drain_end_{};
    bool lanes_done_ = false;
    SetupPhase setup_phase_ = SetupPhase::None;
    fiber::common::IoErr setup_error_ = fiber::common::IoErr::None;
    fiber::http::Http3ClientConnectError connect_error_{};
};

[[nodiscard]] std::uint64_t scheduled_before(std::chrono::nanoseconds elapsed, std::uint64_t rps) noexcept {
    if (elapsed.count() <= 0 || rps == 0) {
        return 0;
    }
    const unsigned __int128 numerator = static_cast<unsigned __int128>(elapsed.count()) * rps + 1000000000ULL - 1;
    return static_cast<std::uint64_t>(numerator / 1000000000ULL);
}

[[nodiscard]] std::uint64_t offered_requests(const BenchmarkOptions &options,
                                             std::chrono::nanoseconds measurement_elapsed) noexcept {
    if (options.mode == LoadMode::Closed) {
        return 0;
    }
    const auto before_start =
            scheduled_before(std::chrono::duration_cast<std::chrono::nanoseconds>(options.warmup), options.rps);
    const auto before_end = scheduled_before(
            std::chrono::duration_cast<std::chrono::nanoseconds>(options.warmup) + measurement_elapsed, options.rps);
    return before_end - before_start;
}

void print_histogram(std::ostream &out, std::string_view name, const LatencyHistogram &histogram) {
    if (histogram.count() == 0) {
        out << "  " << name << ": no successful samples\n";
        return;
    }
    out << "  " << name << " (us): p50=" << histogram.percentile(0.50) << " p90=" << histogram.percentile(0.90)
        << " p99=" << histogram.percentile(0.99) << " p99.9=" << histogram.percentile(0.999)
        << " max=" << histogram.max_us() << '\n';
}

void print_summary(std::ostream &out, const BenchmarkOptions &options, const WorkerStats &stats,
                   const std::vector<std::unique_ptr<BenchmarkWorker>> &workers, std::uint64_t offered,
                   std::chrono::nanoseconds measurement_elapsed) {
    const double seconds = std::chrono::duration<double>(measurement_elapsed).count();
    const double rps = seconds > 0 ? static_cast<double>(stats.succeeded) / seconds : 0.0;
    const double mib_per_second =
            seconds > 0 ? static_cast<double>(stats.response_bytes) / (1024.0 * 1024.0) / seconds : 0.0;
    const std::uint64_t effective_offered = options.mode == LoadMode::Rate ? offered : stats.started;
    const double completion =
            effective_offered == 0 ? 0.0 : static_cast<double>(stats.finished) / static_cast<double>(effective_offered);

    out << "HTTP/3 benchmark summary\n"
        << "  target: " << options.target.url << " -> " << options.target.remote.to_string() << '\n'
        << "  method: " << method_name(options.method) << " mode=" << mode_name(options.mode)
        << " threads=" << options.threads << " connections=" << options.connections
        << " streams/connection=" << options.streams << " pacing=" << (options.pacing_enabled ? "on" : "off") << '\n'
        << "  warmup=" << options.warmup.count() << "ms duration=" << options.duration.count()
        << "ms measurement_elapsed="
        << std::chrono::duration_cast<std::chrono::milliseconds>(measurement_elapsed).count()
        << "ms drain=" << options.drain.count() << "ms timeout=" << options.request_timeout.count() << "ms\n";
    if (options.mode == LoadMode::Rate) {
        out << "  target_rps=" << options.rps << '\n';
    }
    out << "  offered=" << effective_offered << " started=" << stats.started << " finished=" << stats.finished
        << " succeeded=" << stats.succeeded << " failed=" << stats.failed << " completion=" << std::fixed
        << std::setprecision(4) << completion << '\n'
        << "  throughput=" << std::setprecision(2) << rps << " req/s, " << mib_per_second << " MiB/s"
        << " response_bytes=" << stats.response_bytes << '\n'
        << "  warmup_started=" << stats.warmup_started << " warmup_errors=" << stats.warmup_errors << '\n'
        << "  lane_stops: terminal=" << stats.terminal_lane_stops << " failure_guard=" << stats.failure_guard_lane_stops
        << '\n'
        << "  status: other=" << stats.statuses[0] << " 1xx=" << stats.statuses[1] << " 2xx=" << stats.statuses[2]
        << " 3xx=" << stats.statuses[3] << " 4xx=" << stats.statuses[4] << " 5xx=" << stats.statuses[5] << '\n';

    print_histogram(out, "ttfb", stats.ttfb);
    print_histogram(out, "total", stats.total);
    if (options.mode == LoadMode::Rate) {
        print_histogram(out, "queue_delay", stats.queue_delay);
        print_histogram(out, "corrected", stats.corrected);
    }

    out << "  request errors:";
    bool any_error = false;
    for (std::size_t i = 0; i < stats.phase_errors.size(); ++i) {
        if (stats.phase_errors[i] != 0) {
            out << ' ' << request_phase_name(static_cast<RequestPhase>(i)) << '=' << stats.phase_errors[i];
            any_error = true;
        }
    }
    out << (any_error ? "\n" : " none\n");

    if (stats.warmup_errors != 0) {
        out << "  warmup request errors:";
        for (std::size_t i = 0; i < stats.warmup_phase_errors.size(); ++i) {
            if (stats.warmup_phase_errors[i] != 0) {
                out << ' ' << request_phase_name(static_cast<RequestPhase>(i)) << '=' << stats.warmup_phase_errors[i];
            }
        }
        out << "\n  warmup io errors:";
        for (std::size_t i = 1; i < stats.warmup_io_errors.size(); ++i) {
            if (stats.warmup_io_errors[i] != 0) {
                out << ' ' << fiber::common::io_err_name(static_cast<fiber::common::IoErr>(i)) << '='
                    << stats.warmup_io_errors[i];
            }
        }
        out << '\n';
    }

    out << "  io errors:";
    any_error = false;
    for (std::size_t i = 1; i < stats.io_errors.size(); ++i) {
        if (stats.io_errors[i] != 0) {
            out << ' ' << fiber::common::io_err_name(static_cast<fiber::common::IoErr>(i)) << '=' << stats.io_errors[i];
            any_error = true;
        }
    }
    out << (any_error ? "\n" : " none\n");

    out << "  outcomes:";
    for (std::size_t i = 0; i < stats.outcomes.size(); ++i) {
        out << ' ' << outcome_name(i) << '=' << stats.outcomes[i];
    }
    out << '\n'
        << "  endpoint: dropped_datagrams=" << stats.endpoint_dropped_datagrams
        << " recv_storage_high_water=" << stats.endpoint_recv_storage_high_water
        << " recv_storage_rejected=" << stats.endpoint_recv_storage_rejected << '\n'
        << "  quic: path_sent_bytes=" << stats.path_sent_bytes << " path_received_bytes=" << stats.path_received_bytes
        << " app_packets_started=" << stats.app_packets_started << " key_updates=" << stats.key_updates
        << " current_key_packets=" << stats.current_key_packets << " cwnd_bytes=" << stats.cwnd_bytes
        << " in_flight_bytes=" << stats.in_flight_bytes << " pto_count=" << stats.pto_count;
    if (stats.rtt_samples != 0) {
        out << " avg_rtt_ms=" << static_cast<double>(stats.rtt_sum_ms) / static_cast<double>(stats.rtt_samples);
    }
    out << '\n';

    out << "  client endpoints:";
    for (std::size_t i = 0; i < workers.size(); ++i) {
        out << " worker" << i << '=' << workers[i]->local_address();
    }
    out << '\n';
}

void write_json_string(std::ostream &out, std::string_view value) {
    out << '"';
    for (const unsigned char ch: value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(ch)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    out << '"';
}

void write_histogram_json(std::ostream &out, const LatencyHistogram &histogram) {
    out << "{\"samples\":" << histogram.count() << ",\"p50_us\":" << histogram.percentile(0.50)
        << ",\"p90_us\":" << histogram.percentile(0.90) << ",\"p99_us\":" << histogram.percentile(0.99)
        << ",\"p999_us\":" << histogram.percentile(0.999) << ",\"max_us\":" << histogram.max_us() << '}';
}

[[nodiscard]] bool write_json_summary(const BenchmarkOptions &options, const WorkerStats &stats, std::uint64_t offered,
                                      std::chrono::nanoseconds measurement_elapsed, std::string &error) {
    if (options.json_file.empty()) {
        return true;
    }
    std::ofstream out(options.json_file, std::ios::trunc);
    if (!out) {
        error = "failed to open JSON output: " + options.json_file;
        return false;
    }
    const std::uint64_t effective_offered = options.mode == LoadMode::Rate ? offered : stats.started;
    const double seconds = std::chrono::duration<double>(measurement_elapsed).count();
    const double completion =
            effective_offered == 0 ? 0.0 : static_cast<double>(stats.finished) / static_cast<double>(effective_offered);
    const double requests_per_second = seconds > 0 ? static_cast<double>(stats.succeeded) / seconds : 0.0;
    const double mib_per_second =
            seconds > 0 ? static_cast<double>(stats.response_bytes) / (1024.0 * 1024.0) / seconds : 0.0;
    out << "{\n  \"target\":";
    write_json_string(out, options.target.url);
    out << ",\n  \"remote\":";
    write_json_string(out, options.target.remote.to_string());
    out << ",\n  \"mode\":";
    write_json_string(out, mode_name(options.mode));
    out << ",\n  \"method\":";
    write_json_string(out, method_name(options.method));
    out << ",\n  \"pacing\":" << (options.pacing_enabled ? "true" : "false") << ",\n  \"threads\":" << options.threads
        << ",\n  \"connections\":" << options.connections << ",\n  \"streams_per_connection\":" << options.streams
        << ",\n  \"warmup_ms\":" << options.warmup.count() << ",\n  \"duration_ms\":" << options.duration.count()
        << ",\n  \"offered\":" << effective_offered
        << ",\n  \"measurement_elapsed_ms\":" << std::chrono::duration<double, std::milli>(measurement_elapsed).count()
        << ",\n  \"started\":" << stats.started << ",\n  \"finished\":" << stats.finished
        << ",\n  \"succeeded\":" << stats.succeeded << ",\n  \"failed\":" << stats.failed
        << ",\n  \"completion\":" << completion << ",\n  \"requests_per_second\":" << requests_per_second
        << ",\n  \"mib_per_second\":" << mib_per_second << ",\n  \"response_bytes\":" << stats.response_bytes
        << ",\n  \"warmup_started\":" << stats.warmup_started << ",\n  \"warmup_errors\":" << stats.warmup_errors
        << ",\n  \"terminal_lane_stops\":" << stats.terminal_lane_stops
        << ",\n  \"failure_guard_lane_stops\":" << stats.failure_guard_lane_stops
        << ",\n  \"latency\":{\n    \"ttfb\":";
    write_histogram_json(out, stats.ttfb);
    out << ",\n    \"total\":";
    write_histogram_json(out, stats.total);
    out << ",\n    \"queue_delay\":";
    write_histogram_json(out, stats.queue_delay);
    out << ",\n    \"corrected\":";
    write_histogram_json(out, stats.corrected);
    out << "\n  },\n  \"phase_errors\":{";
    for (std::size_t i = 0; i < stats.phase_errors.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        write_json_string(out, request_phase_name(static_cast<RequestPhase>(i)));
        out << ':' << stats.phase_errors[i];
    }
    out << "},\n  \"io_errors\":{";
    for (std::size_t i = 0; i < stats.io_errors.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        write_json_string(out, fiber::common::io_err_name(static_cast<fiber::common::IoErr>(i)));
        out << ':' << stats.io_errors[i];
    }
    out << "},\n  \"warmup_phase_errors\":{";
    for (std::size_t i = 0; i < stats.warmup_phase_errors.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        write_json_string(out, request_phase_name(static_cast<RequestPhase>(i)));
        out << ':' << stats.warmup_phase_errors[i];
    }
    out << "},\n  \"warmup_io_errors\":{";
    for (std::size_t i = 0; i < stats.warmup_io_errors.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        write_json_string(out, fiber::common::io_err_name(static_cast<fiber::common::IoErr>(i)));
        out << ':' << stats.warmup_io_errors[i];
    }
    out << "},\n  \"statuses\":{"
        << "\"other\":" << stats.statuses[0] << ",\"1xx\":" << stats.statuses[1] << ",\"2xx\":" << stats.statuses[2]
        << ",\"3xx\":" << stats.statuses[3] << ",\"4xx\":" << stats.statuses[4] << ",\"5xx\":" << stats.statuses[5]
        << "},\n  \"outcomes\":{";
    for (std::size_t i = 0; i < stats.outcomes.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        write_json_string(out, outcome_name(i));
        out << ':' << stats.outcomes[i];
    }
    out << "},\n  \"endpoint\":{\"dropped_datagrams\":" << stats.endpoint_dropped_datagrams
        << ",\"recv_storage_high_water\":" << stats.endpoint_recv_storage_high_water
        << ",\"recv_storage_rejected\":" << stats.endpoint_recv_storage_rejected << "},\n  \"quic\":{"
        << "\"path_sent_bytes\":" << stats.path_sent_bytes << ",\"path_received_bytes\":" << stats.path_received_bytes
        << ",\"app_packets_started\":" << stats.app_packets_started << ",\"key_updates\":" << stats.key_updates
        << ",\"current_key_packets\":" << stats.current_key_packets << ",\"cwnd_bytes\":" << stats.cwnd_bytes
        << ",\"in_flight_bytes\":" << stats.in_flight_bytes << ",\"pto_count\":" << stats.pto_count
        << ",\"rtt_samples\":" << stats.rtt_samples << ",\"avg_rtt_ms\":"
        << (stats.rtt_samples == 0 ? 0.0
                                   : static_cast<double>(stats.rtt_sum_ms) / static_cast<double>(stats.rtt_samples))
        << "}\n}\n";
    if (!out) {
        error = "failed to write JSON output: " + options.json_file;
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    BenchmarkOptions options;
    std::string error;
    bool show_help = false;
    if (!parse_options(argc, argv, options, error, show_help)) {
        std::cerr << "error: " << error << "\n\n";
        print_usage(std::cerr);
        return 1;
    }
    if (show_help) {
        print_usage(std::cout);
        return 0;
    }

    (void) std::signal(SIGINT, on_signal);
    (void) std::signal(SIGTERM, on_signal);
    (void) std::signal(SIGPIPE, SIG_IGN);

    RunCoordinator coordinator;
    coordinator.worker_count = options.threads;
    auto record_signal = [&coordinator]() noexcept {
        if (g_stop_requested == 0) {
            return;
        }
        std::int64_t expected = 0;
        const std::int64_t stopped_at = time_point_ns(std::chrono::steady_clock::now());
        (void) coordinator.measurement_stop_ns.compare_exchange_strong(expected, stopped_at, std::memory_order_release,
                                                                       std::memory_order_relaxed);
        coordinator.stop.store(true, std::memory_order_release);
    };
    fiber::event::EventLoopGroup group(options.threads);
    group.start();

    std::vector<std::unique_ptr<BenchmarkWorker>> workers;
    workers.reserve(options.threads);
    const std::size_t base_connections = options.connections / options.threads;
    const std::size_t extra_connections = options.connections % options.threads;
    for (std::size_t i = 0; i < options.threads; ++i) {
        const std::size_t worker_connections = base_connections + (i < extra_connections ? 1 : 0);
        workers.push_back(std::make_unique<BenchmarkWorker>(i, worker_connections, group.at(i), options, coordinator));
        fiber::async::spawn(group.at(i), [worker = workers.back().get()]() { return worker->run(); });
    }

    while (coordinator.setup_done.load(std::memory_order_acquire) != options.threads) {
        record_signal();
        std::this_thread::sleep_for(1ms);
    }

    if (coordinator.setup_failed.load(std::memory_order_acquire)) {
        coordinator.stop.store(true, std::memory_order_release);
    } else if (g_stop_requested == 0) {
        const auto start = std::chrono::steady_clock::now() + 100ms;
        coordinator.warmup_start_ns.store(time_point_ns(start), std::memory_order_release);
    } else {
        record_signal();
    }

    while (coordinator.workers_done.load(std::memory_order_acquire) != options.threads) {
        record_signal();
        std::this_thread::sleep_for(10ms);
    }
    record_signal();

    group.stop();
    group.join();

    if (coordinator.setup_failed.load(std::memory_order_acquire)) {
        for (std::size_t i = 0; i < workers.size(); ++i) {
            if (workers[i]->setup_phase() == SetupPhase::None) {
                continue;
            }
            std::cerr << "worker " << i << " setup failed in " << setup_phase_name(workers[i]->setup_phase()) << ": "
                      << fiber::common::io_err_name(workers[i]->setup_error());
            if (workers[i]->setup_phase() == SetupPhase::Connect) {
                const auto &connect_error = workers[i]->connect_error();
                std::cerr << " http3_phase=" << http3_connect_phase_name(connect_error.phase)
                          << " quic_phase=" << quic_connect_phase_name(connect_error.quic_error.phase)
                          << " tls_verify_result=" << connect_error.quic_error.tls_verify_result
                          << " tls_alert=" << static_cast<unsigned>(connect_error.quic_error.tls_alert);
            }
            std::cerr << '\n';
        }
        return 1;
    }

    WorkerStats aggregate;
    for (const auto &worker: workers) {
        aggregate.merge(worker->stats());
    }
    auto measurement_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(options.duration);
    if (g_stop_requested != 0) {
        const std::int64_t warmup_start_ns = coordinator.warmup_start_ns.load(std::memory_order_acquire);
        const std::int64_t stopped_at_ns = coordinator.measurement_stop_ns.load(std::memory_order_acquire);
        if (warmup_start_ns == 0 || stopped_at_ns == 0) {
            measurement_elapsed = 0ns;
        } else {
            const auto measurement_start = time_point_from_ns(warmup_start_ns) + options.warmup;
            const auto stopped_at = time_point_from_ns(stopped_at_ns);
            measurement_elapsed = stopped_at > measurement_start
                                          ? std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                             stopped_at - measurement_start),
                                                     measurement_elapsed)
                                          : 0ns;
        }
    }
    const std::uint64_t offered = offered_requests(options, measurement_elapsed);
    print_summary(std::cout, options, aggregate, workers, offered, measurement_elapsed);
    if (!write_json_summary(options, aggregate, offered, measurement_elapsed, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    if (g_stop_requested != 0) {
        return 130;
    }
    return aggregate.failed == 0 && aggregate.warmup_errors == 0 ? 0 : 2;
}
