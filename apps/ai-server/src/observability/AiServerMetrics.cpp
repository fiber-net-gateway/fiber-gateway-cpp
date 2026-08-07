#include "AiServerMetrics.h"
#include "ProcessMetrics.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/common/mem/IoBuf.h>

namespace fiber::ai_server {
namespace {

constexpr std::size_t kMaxMetricsOutputBytes = 16 * 1024 * 1024;
constexpr std::array<std::string_view, 2> kProtocolNames{"openai", "anthropic"};
constexpr std::array<std::string_view, 3> kTokenTypes{"in_cache", "in_nocache", "out"};
constexpr std::array<std::string_view, static_cast<std::size_t>(ProviderHttpErrorCode::Count)> kFailurePhases{
        "invalid_endpoint", "no_service_endpoint", "dns",       "pool_shutdown",      "connect",          "send_header",
        "send_body",        "read_header",         "read_body", "response_too_large", "invalid_response",
};
constexpr std::string_view kUserTokenUsageMetric = "ai_server_user_token_usage_total";
constexpr std::string_view kProviderTokenUsageMetric = "ai_server_provider_token_usage_total";
constexpr std::size_t kMetricsChunkBytes = 4 * 1024;
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;

constexpr std::size_t protocol_index(LlmWireProtocol protocol) noexcept {
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? 0 : 1;
}

std::size_t request_result_index(const http::HttpResponseStats &response) noexcept {
    if (response.terminal_error != common::IoErr::None || !response.completed) {
        return 3;
    }
    if (response.status_code >= 200 && response.status_code < 400) {
        return 0;
    }
    if (response.status_code >= 400 && response.status_code < 500) {
        return 1;
    }
    return 2;
}

class BoundedTextBuilder {
public:
    BoundedTextBuilder(mem::IoBufChain &output, std::size_t max_size) noexcept :
        output_(&output), max_size_(max_size), written_(output.readable_bytes()) {}

    [[nodiscard]] bool append(std::string_view value) noexcept {
        if (error_ != common::IoErr::None) {
            return false;
        }
        if (written_ > max_size_ || value.size() > max_size_ - written_) {
            error_ = common::IoErr::MessageTooLarge;
            return false;
        }
        while (!value.empty()) {
            mem::IoBuf *tail = output_->back();
            if (!tail || tail->writable() == 0) {
                const std::size_t capacity = std::min(kMetricsChunkBytes, max_size_ - written_);
                mem::IoBuf chunk = mem::IoBuf::allocate(capacity);
                if (!chunk || !output_->append(std::move(chunk))) {
                    error_ = common::IoErr::NoMem;
                    return false;
                }
                tail = output_->back();
            }
            const std::size_t count = std::min(value.size(), tail->writable());
            std::memcpy(tail->writable_data(), value.data(), count);
            output_->commit_back(count);
            written_ += count;
            value.remove_prefix(count);
        }
        return true;
    }

    [[nodiscard]] bool append_uint(std::uint64_t value) noexcept {
        std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (converted.ec != std::errc{}) {
            error_ = common::IoErr::Invalid;
            return false;
        }
        return append(std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }

    [[nodiscard]] bool append_seconds(std::uint64_t nanoseconds) noexcept {
        if (!append_uint(nanoseconds / kNanosecondsPerSecond)) {
            return false;
        }
        const std::uint64_t remainder = nanoseconds % kNanosecondsPerSecond;
        if (remainder == 0) {
            return true;
        }

        std::array<char, 9> fraction{};
        std::uint64_t value = remainder;
        for (std::size_t index = fraction.size(); index > 0; --index) {
            fraction[index - 1] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        std::size_t length = fraction.size();
        while (length > 0 && fraction[length - 1] == '0') {
            --length;
        }
        return append(".") && append(std::string_view(fraction.data(), length));
    }

    [[nodiscard]] bool append_label_value(std::string_view value) noexcept {
        std::size_t start = 0;
        for (std::size_t index = 0; index < value.size(); ++index) {
            const char ch = value[index];
            if (ch != '\\' && ch != '"' && ch != '\n') {
                continue;
            }
            if (!append(value.substr(start, index - start)) ||
                !append(ch == '\\' ? std::string_view("\\\\")
                                   : (ch == '"' ? std::string_view("\\\"") : std::string_view("\\n")))) {
                return false;
            }
            start = index + 1;
        }
        return append(value.substr(start));
    }

    [[nodiscard]] common::IoErr error() const noexcept { return error_; }

private:
    mem::IoBufChain *output_ = nullptr;
    std::size_t max_size_ = 0;
    std::size_t written_ = 0;
    common::IoErr error_ = common::IoErr::None;
};

[[nodiscard]] bool append_gauge(BoundedTextBuilder &output, std::string_view name, std::string_view help,
                                std::uint64_t value) noexcept {
    return output.append("# HELP ") && output.append(name) && output.append(" ") && output.append(help) &&
           output.append("\n# TYPE ") && output.append(name) && output.append(" gauge\n") && output.append(name) &&
           output.append(" ") && output.append_uint(value) && output.append("\n");
}

[[nodiscard]] bool append_counter(BoundedTextBuilder &output, std::string_view name, std::string_view help,
                                  std::uint64_t value) noexcept {
    return output.append("# HELP ") && output.append(name) && output.append(" ") && output.append(help) &&
           output.append("\n# TYPE ") && output.append(name) && output.append(" counter\n") && output.append(name) &&
           output.append(" ") && output.append_uint(value) && output.append("\n");
}

[[nodiscard]] bool append_seconds_metric(BoundedTextBuilder &output, std::string_view name, std::string_view help,
                                         std::string_view type, std::uint64_t nanoseconds) noexcept {
    return output.append("# HELP ") && output.append(name) && output.append(" ") && output.append(help) &&
           output.append("\n# TYPE ") && output.append(name) && output.append(" ") && output.append(type) &&
           output.append("\n") && output.append(name) && output.append(" ") && output.append_seconds(nanoseconds) &&
           output.append("\n");
}

[[nodiscard]] bool append_process_metrics(BoundedTextBuilder &output) noexcept {
    const ProcessMetricsSnapshot metrics = collect_process_metrics();
    if (metrics.cpu_time_nanoseconds &&
        !append_seconds_metric(output, "process_cpu_seconds_total", "Total user and system CPU time spent in seconds.",
                               "counter", *metrics.cpu_time_nanoseconds)) {
        return false;
    }
    if (metrics.resident_memory_bytes &&
        !append_gauge(output, "process_resident_memory_bytes", "Resident memory size in bytes.",
                      *metrics.resident_memory_bytes)) {
        return false;
    }
    if (metrics.virtual_memory_bytes && !append_gauge(output, "process_virtual_memory_bytes",
                                                      "Virtual memory size in bytes.", *metrics.virtual_memory_bytes)) {
        return false;
    }
    if (metrics.start_time_nanoseconds &&
        !append_seconds_metric(output, "process_start_time_seconds",
                               "Start time of the process since unix epoch in seconds.", "gauge",
                               *metrics.start_time_nanoseconds)) {
        return false;
    }
    if (metrics.open_fds &&
        !append_gauge(output, "process_open_fds", "Number of open file descriptors.", *metrics.open_fds)) {
        return false;
    }
    if (metrics.max_fds &&
        !append_gauge(output, "process_max_fds", "Maximum number of open file descriptors.", *metrics.max_fds)) {
        return false;
    }
    return true;
}

struct TokenUsageCounters {
    std::array<std::atomic<std::uint64_t>, 3> values{};
    std::atomic<bool> observed{false};

    void add(const LlmTokenUsage &usage) noexcept {
        add_value(0, usage.in_cache);
        add_value(1, usage.in_nocache);
        add_value(2, usage.out);
        observed.store(true, std::memory_order_release);
    }

private:
    void add_value(std::size_t index, const std::optional<std::int64_t> &value) noexcept {
        if (value && *value >= 0) {
            values[index].fetch_add(static_cast<std::uint64_t>(*value), std::memory_order_relaxed);
        }
    }
};

struct UserTokenUsageSeries {
    explicit UserTokenUsageSeries(std::string_view value) : username(value) {}

    std::string username;
    TokenUsageCounters usage;
};

struct ProviderTokenUsageSeries {
    explicit ProviderTokenUsageSeries(std::string_view value) : provider_name(value) {}

    std::string provider_name;
    std::array<TokenUsageCounters, 2> protocols;
};

[[nodiscard]] bool append_family_header(BoundedTextBuilder &output, std::string_view name, std::string_view help) {
    return output.append("# HELP ") && output.append(name) && output.append(" ") && output.append(help) &&
           output.append("\n# TYPE ") && output.append(name) && output.append(" counter\n");
}

[[nodiscard]] bool append_user_sample(BoundedTextBuilder &output, std::string_view username,
                                      std::string_view token_type, std::uint64_t value) {
    return output.append(kUserTokenUsageMetric) && output.append("{username=\"") &&
           output.append_label_value(username) && output.append("\",token_type=\"") && output.append(token_type) &&
           output.append("\"} ") && output.append_uint(value) && output.append("\n");
}

[[nodiscard]] bool append_provider_sample(BoundedTextBuilder &output, std::string_view provider_name,
                                          std::string_view protocol, std::string_view token_type, std::uint64_t value) {
    return output.append(kProviderTokenUsageMetric) && output.append("{provider_name=\"") &&
           output.append_label_value(provider_name) && output.append("\",protocol=\"") && output.append(protocol) &&
           output.append("\",token_type=\"") && output.append(token_type) && output.append("\"} ") &&
           output.append_uint(value) && output.append("\n");
}

} // namespace

class AiServerMetrics::WorkerTokenUsageCache {
public:
    std::unordered_map<std::string_view, UserTokenUsageSeries *> users;
    std::unordered_map<std::string_view, ProviderTokenUsageSeries *> providers;
};

class AiServerMetrics::TokenUsageStore {
public:
    void record(WorkerTokenUsageCache &cache, std::string_view username, std::string_view provider_name,
                LlmWireProtocol protocol, const LlmTokenUsage &usage) {
        if (!usage.has_usage_fields()) {
            return;
        }
        find_user(cache, username)->usage.add(usage);
        find_provider(cache, provider_name)->protocols[protocol_index(protocol)].add(usage);
    }

    [[nodiscard]] bool append_text(BoundedTextBuilder &output) {
        if (!append_family_header(output, kUserTokenUsageMetric,
                                  "LLM tokens by authenticated username and token type.")) {
            return false;
        }

        std::lock_guard lock(mutex_);
        for (const auto &[username, series]: users_) {
            for (std::size_t type = 0; type < kTokenTypes.size(); ++type) {
                if (!append_user_sample(output, username, kTokenTypes[type],
                                        series->usage.values[type].load(std::memory_order_relaxed))) {
                    return false;
                }
            }
        }

        if (!append_family_header(output, kProviderTokenUsageMetric,
                                  "LLM tokens by provider, wire protocol, and token type.")) {
            return false;
        }
        for (const auto &[provider_name, series]: providers_) {
            for (std::size_t protocol = 0; protocol < kProtocolNames.size(); ++protocol) {
                const TokenUsageCounters &usage = series->protocols[protocol];
                if (!usage.observed.load(std::memory_order_acquire)) {
                    continue;
                }
                for (std::size_t type = 0; type < kTokenTypes.size(); ++type) {
                    if (!append_provider_sample(output, provider_name, kProtocolNames[protocol], kTokenTypes[type],
                                                usage.values[type].load(std::memory_order_relaxed))) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

private:
    UserTokenUsageSeries *find_user(WorkerTokenUsageCache &cache, std::string_view username) {
        const auto cached = cache.users.find(username);
        if (cached != cache.users.end()) {
            return cached->second;
        }

        UserTokenUsageSeries *series = nullptr;
        {
            std::lock_guard lock(mutex_);
            auto found = users_.find(username);
            if (found == users_.end()) {
                auto created = std::make_unique<UserTokenUsageSeries>(username);
                series = created.get();
                users_.emplace(series->username, std::move(created));
            } else {
                series = found->second.get();
            }
        }
        cache.users.emplace(series->username, series);
        return series;
    }

    ProviderTokenUsageSeries *find_provider(WorkerTokenUsageCache &cache, std::string_view provider_name) {
        const auto cached = cache.providers.find(provider_name);
        if (cached != cache.providers.end()) {
            return cached->second;
        }

        ProviderTokenUsageSeries *series = nullptr;
        {
            std::lock_guard lock(mutex_);
            auto found = providers_.find(provider_name);
            if (found == providers_.end()) {
                auto created = std::make_unique<ProviderTokenUsageSeries>(provider_name);
                series = created.get();
                providers_.emplace(series->provider_name, std::move(created));
            } else {
                series = found->second.get();
            }
        }
        cache.providers.emplace(series->provider_name, series);
        return series;
    }

    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<UserTokenUsageSeries>, std::less<>> users_;
    std::map<std::string, std::unique_ptr<ProviderTokenUsageSeries>, std::less<>> providers_;
};

AiServerMetrics::Worker::Worker() : token_usage_cache_(std::make_unique<WorkerTokenUsageCache>()) {}

AiServerMetrics::Worker::~Worker() = default;

AiServerMetrics::Worker::Worker(Worker &&) noexcept = default;

AiServerMetrics::Worker &AiServerMetrics::Worker::operator=(Worker &&) noexcept = default;

void AiServerMetrics::Worker::request_started(LlmWireProtocol protocol) noexcept {
    inflight_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::request_finished(LlmWireProtocol protocol, const http::HttpResponseStats &response,
                                               std::chrono::microseconds duration) noexcept {
    const std::size_t index = protocol_index(protocol);
    inflight_[index].dec();
    requests_[index][request_result_index(response)].inc();
    request_duration_[index].observe(static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count(), 0)));
}

void AiServerMetrics::Worker::provider_attempt(LlmWireProtocol protocol) noexcept {
    provider_attempts_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::provider_failure(LlmWireProtocol protocol) noexcept {
    provider_failures_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::provider_retry(LlmWireProtocol protocol) noexcept {
    provider_retries_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::provider_transport_failure(LlmWireProtocol protocol,
                                                         ProviderHttpErrorCode phase) noexcept {
    const std::size_t phase_index = static_cast<std::size_t>(phase);
    if (phase_index < static_cast<std::size_t>(ProviderHttpErrorCode::Count)) {
        provider_transport_failures_[protocol_index(protocol)][phase_index].inc();
    }
}

void AiServerMetrics::Worker::provider_attempts_skipped(LlmWireProtocol protocol, std::size_t count) noexcept {
    provider_attempts_skipped_[protocol_index(protocol)].add(count);
}

void AiServerMetrics::Worker::dns_backoff_hit(LlmWireProtocol protocol) noexcept {
    dns_backoff_hits_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::provider_circuit_open(LlmWireProtocol protocol) noexcept {
    provider_circuit_opens_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::rate_limit_check(RateLimitCheckMetric result) noexcept {
    rate_limit_checks_[static_cast<std::size_t>(result)].inc();
}

void AiServerMetrics::Worker::rate_limit_settle(RateLimitSettleMetric result) noexcept {
    rate_limit_settles_[static_cast<std::size_t>(result)].inc();
}

void AiServerMetrics::Worker::sse_failure(LlmWireProtocol protocol) noexcept {
    sse_failures_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::sse_drain(LlmWireProtocol protocol, SseDrainMetric result) noexcept {
    sse_drains_[protocol_index(protocol)][static_cast<std::size_t>(result)].inc();
}

void AiServerMetrics::Worker::audit_generated() noexcept { audit_generated_.inc(); }

void AiServerMetrics::Worker::audit_generation_failed() noexcept { audit_generation_failures_.inc(); }

void AiServerMetrics::Worker::audit_capture_incomplete() noexcept { audit_capture_incomplete_.inc(); }

void AiServerMetrics::Worker::token_usage(std::string_view username, std::string_view provider_name,
                                          LlmWireProtocol protocol, const LlmTokenUsage &usage) noexcept {
    FIBER_ASSERT(owner_ != nullptr);
    owner_->record_token_usage(*this, username, provider_name, protocol, usage);
}

AiServerMetrics::AiServerMetrics(event::EventLoopGroup &workers) :
    token_usage_store_(std::make_unique<TokenUsageStore>()) {
    valid_ = initialize(workers);
}

AiServerMetrics::~AiServerMetrics() { FIBER_ASSERT(!valid_ || collecting_stopped_); }

bool AiServerMetrics::initialize(event::EventLoopGroup &worker_group) {
    constexpr std::array<std::string_view, 1> kProtocolLabel{"protocol"};
    constexpr std::array<std::string_view, 2> kRequestLabels{"protocol", "result"};
    constexpr std::array<std::string_view, 2> kTransportFailureLabels{"protocol", "phase"};
    constexpr std::array<std::string_view, 2> kSseDrainLabels{"protocol", "result"};
    constexpr std::array<std::string_view, 1> kResultLabel{"result"};
    constexpr std::array<std::string_view, 4> kRequestResults{"success", "client_error", "server_error", "canceled"};
    constexpr std::array<std::string_view, 4> kCheckResults{"bypass", "allowed", "denied", "error"};
    constexpr std::array<std::string_view, 3> kSettleResults{"usage", "no_usage", "error"};
    constexpr std::array<std::string_view, 3> kSseDrainResults{"completed", "upstream_error", "timeout"};
    constexpr std::array<std::uint64_t, 15> kDurationBounds{
            1000,    5000,    10000,   25000,    50000,    100000,   250000,    500000,
            1000000, 2500000, 5000000, 10000000, 30000000, 60000000, 300000000,
    };

    auto requests = registry_.register_counter("ai_server_requests_total", "Completed LLM requests.", kRequestLabels);
    auto duration =
            registry_.register_histogram("ai_server_request_duration_seconds", "LLM request duration.", kDurationBounds,
                                         prometheus::HistogramUnit::Microseconds, kProtocolLabel);
    auto inflight = registry_.register_gauge("ai_server_requests_inflight", "In-flight LLM requests.",
                                             prometheus::GaugeReduction::Sum, kProtocolLabel);
    auto provider_attempts =
            registry_.register_counter("ai_server_provider_attempts_total", "Provider attempts.", kProtocolLabel);
    auto provider_failures = registry_.register_counter("ai_server_provider_failures_total",
                                                        "Failed Provider attempts.", kProtocolLabel);
    auto provider_retries = registry_.register_counter("ai_server_provider_retries_total", "Retried Provider attempts.",
                                                       kProtocolLabel);
    auto provider_transport_failures =
            registry_.register_counter("ai_server_provider_transport_failures_total",
                                       "Provider transport failures by phase.", kTransportFailureLabels);
    auto provider_attempts_skipped = registry_.register_counter(
            "ai_server_provider_attempts_skipped_total", "Provider attempts skipped by retry pruning.", kProtocolLabel);
    auto dns_backoff_hits =
            registry_.register_counter("ai_server_dns_backoff_hits_total",
                                       "Provider DNS lookups suppressed by transient timeout backoff.", kProtocolLabel);
    auto provider_circuit_opens = registry_.register_counter("ai_server_provider_circuit_opens_total",
                                                             "Provider circuit breaker openings.", kProtocolLabel);
    auto rate_checks = registry_.register_counter("ai_server_rate_limit_checks_total",
                                                  "Token rate limit check outcomes.", kResultLabel);
    auto rate_settles = registry_.register_counter("ai_server_rate_limit_settlements_total",
                                                   "Token rate limit settlements.", kResultLabel);
    auto sse_failures = registry_.register_counter("ai_server_sse_failures_total", "SSE relay or delivery failures.",
                                                   kProtocolLabel);
    auto sse_drains = registry_.register_counter(
            "ai_server_sse_drains_total", "Upstream SSE drains after downstream delivery failure.", kSseDrainLabels);
    auto audit_generated =
            registry_.register_counter("ai_server_audit_generated_records_total", "LLM audit records generated.");
    auto audit_generation_failures = registry_.register_counter("ai_server_audit_generation_failures_total",
                                                                "LLM audit records discarded during generation.");
    auto audit_capture_incomplete = registry_.register_counter(
            "ai_server_audit_capture_incomplete_total", "LLM audit records with incomplete request observations.");
    if (!requests || !duration || !inflight || !provider_attempts || !provider_failures || !provider_retries ||
        !provider_transport_failures || !provider_attempts_skipped || !dns_backoff_hits || !provider_circuit_opens ||
        !rate_checks || !rate_settles || !sse_failures || !sse_drains || !audit_generated ||
        !audit_generation_failures || !audit_capture_incomplete) {
        return false;
    }

    auto audit_generated_series = registry_.register_series(*audit_generated);
    auto audit_generation_failure_series = registry_.register_series(*audit_generation_failures);
    auto audit_capture_incomplete_series = registry_.register_series(*audit_capture_incomplete);
    if (!audit_generated_series || !audit_generation_failure_series || !audit_capture_incomplete_series) {
        return false;
    }

    std::array<std::array<prometheus::SeriesId, 4>, 2> request_series;
    std::array<prometheus::SeriesId, 2> duration_series;
    std::array<prometheus::SeriesId, 2> inflight_series;
    std::array<prometheus::SeriesId, 2> attempt_series;
    std::array<prometheus::SeriesId, 2> failure_series;
    std::array<prometheus::SeriesId, 2> retry_series;
    std::array<std::array<prometheus::SeriesId, kFailurePhases.size()>, 2> transport_failure_series;
    std::array<prometheus::SeriesId, 2> attempts_skipped_series;
    std::array<prometheus::SeriesId, 2> dns_backoff_hit_series;
    std::array<prometheus::SeriesId, 2> circuit_open_series;
    std::array<prometheus::SeriesId, 2> sse_series;
    std::array<std::array<prometheus::SeriesId, 3>, 2> sse_drain_series;
    std::array<prometheus::SeriesId, 4> check_series;
    std::array<prometheus::SeriesId, 3> settle_series;
    for (std::size_t protocol = 0; protocol < kProtocolNames.size(); ++protocol) {
        for (std::size_t result = 0; result < kRequestResults.size(); ++result) {
            auto series = registry_.register_series(*requests, std::array<std::string_view, 2>{
                                                                       kProtocolNames[protocol],
                                                                       kRequestResults[result],
                                                               });
            if (!series) {
                return false;
            }
            request_series[protocol][result] = *series;
        }
        auto duration_id =
                registry_.register_series(*duration, std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto inflight_id =
                registry_.register_series(*inflight, std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto attempt_id = registry_.register_series(*provider_attempts,
                                                    std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto failure_id = registry_.register_series(*provider_failures,
                                                    std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto retry_id =
                registry_.register_series(*provider_retries, std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto attempts_skipped_id = registry_.register_series(*provider_attempts_skipped,
                                                             std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto dns_backoff_hit_id =
                registry_.register_series(*dns_backoff_hits, std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto circuit_open_id = registry_.register_series(*provider_circuit_opens,
                                                         std::array<std::string_view, 1>{kProtocolNames[protocol]});
        auto sse_id =
                registry_.register_series(*sse_failures, std::array<std::string_view, 1>{kProtocolNames[protocol]});
        if (!duration_id || !inflight_id || !attempt_id || !failure_id || !retry_id || !attempts_skipped_id ||
            !dns_backoff_hit_id || !circuit_open_id || !sse_id) {
            return false;
        }
        for (std::size_t phase = 0; phase < kFailurePhases.size(); ++phase) {
            auto transport_failure_id = registry_.register_series(
                    *provider_transport_failures,
                    std::array<std::string_view, 2>{kProtocolNames[protocol], kFailurePhases[phase]});
            if (!transport_failure_id) {
                return false;
            }
            transport_failure_series[protocol][phase] = *transport_failure_id;
        }
        for (std::size_t result = 0; result < kSseDrainResults.size(); ++result) {
            auto drain_id = registry_.register_series(
                    *sse_drains, std::array<std::string_view, 2>{kProtocolNames[protocol], kSseDrainResults[result]});
            if (!drain_id) {
                return false;
            }
            sse_drain_series[protocol][result] = *drain_id;
        }
        duration_series[protocol] = *duration_id;
        inflight_series[protocol] = *inflight_id;
        attempt_series[protocol] = *attempt_id;
        failure_series[protocol] = *failure_id;
        retry_series[protocol] = *retry_id;
        attempts_skipped_series[protocol] = *attempts_skipped_id;
        dns_backoff_hit_series[protocol] = *dns_backoff_hit_id;
        circuit_open_series[protocol] = *circuit_open_id;
        sse_series[protocol] = *sse_id;
    }
    for (std::size_t i = 0; i < kCheckResults.size(); ++i) {
        auto series = registry_.register_series(*rate_checks, std::array<std::string_view, 1>{kCheckResults[i]});
        if (!series) {
            return false;
        }
        check_series[i] = *series;
    }
    for (std::size_t i = 0; i < kSettleResults.size(); ++i) {
        auto series = registry_.register_series(*rate_settles, std::array<std::string_view, 1>{kSettleResults[i]});
        if (!series) {
            return false;
        }
        settle_series[i] = *series;
    }

    std::vector<prometheus::ShardId> shard_ids;
    shard_ids.reserve(worker_group.size());
    for (std::size_t i = 0; i < worker_group.size(); ++i) {
        auto shard = registry_.add_shard(worker_group.at(i));
        if (!shard) {
            return false;
        }
        shard_ids.push_back(*shard);
    }
    if (!registry_.freeze()) {
        return false;
    }

    workers_.resize(worker_group.size());
    for (std::size_t worker_index = 0; worker_index < workers_.size(); ++worker_index) {
        prometheus::MetricsShard *shard = registry_.shard(shard_ids[worker_index]);
        if (!shard) {
            return false;
        }
        Worker &worker = workers_[worker_index];
        worker.owner_ = this;
        auto audit_generated_value = shard->counter(*audit_generated_series);
        auto audit_generation_failure_value = shard->counter(*audit_generation_failure_series);
        auto audit_capture_incomplete_value = shard->counter(*audit_capture_incomplete_series);
        if (!audit_generated_value || !audit_generation_failure_value || !audit_capture_incomplete_value) {
            return false;
        }
        worker.audit_generated_ = *audit_generated_value;
        worker.audit_generation_failures_ = *audit_generation_failure_value;
        worker.audit_capture_incomplete_ = *audit_capture_incomplete_value;
        for (std::size_t protocol = 0; protocol < kProtocolNames.size(); ++protocol) {
            for (std::size_t result = 0; result < kRequestResults.size(); ++result) {
                auto value = shard->counter(request_series[protocol][result]);
                if (!value) {
                    return false;
                }
                worker.requests_[protocol][result] = *value;
            }
            auto duration_value = shard->histogram(duration_series[protocol]);
            auto inflight_value = shard->gauge(inflight_series[protocol]);
            auto attempt_value = shard->counter(attempt_series[protocol]);
            auto failure_value = shard->counter(failure_series[protocol]);
            auto retry_value = shard->counter(retry_series[protocol]);
            auto attempts_skipped_value = shard->counter(attempts_skipped_series[protocol]);
            auto dns_backoff_hit_value = shard->counter(dns_backoff_hit_series[protocol]);
            auto circuit_open_value = shard->counter(circuit_open_series[protocol]);
            auto sse_value = shard->counter(sse_series[protocol]);
            if (!duration_value || !inflight_value || !attempt_value || !failure_value || !retry_value ||
                !attempts_skipped_value || !dns_backoff_hit_value || !circuit_open_value || !sse_value) {
                return false;
            }
            worker.request_duration_[protocol] = *duration_value;
            worker.inflight_[protocol] = *inflight_value;
            worker.provider_attempts_[protocol] = *attempt_value;
            worker.provider_failures_[protocol] = *failure_value;
            worker.provider_retries_[protocol] = *retry_value;
            worker.provider_attempts_skipped_[protocol] = *attempts_skipped_value;
            worker.dns_backoff_hits_[protocol] = *dns_backoff_hit_value;
            worker.provider_circuit_opens_[protocol] = *circuit_open_value;
            for (std::size_t phase = 0; phase < kFailurePhases.size(); ++phase) {
                auto transport_failure_value = shard->counter(transport_failure_series[protocol][phase]);
                if (!transport_failure_value) {
                    return false;
                }
                worker.provider_transport_failures_[protocol][phase] = *transport_failure_value;
            }
            worker.sse_failures_[protocol] = *sse_value;
            for (std::size_t result = 0; result < kSseDrainResults.size(); ++result) {
                auto drain_value = shard->counter(sse_drain_series[protocol][result]);
                if (!drain_value) {
                    return false;
                }
                worker.sse_drains_[protocol][result] = *drain_value;
            }
        }
        for (std::size_t i = 0; i < check_series.size(); ++i) {
            auto value = shard->counter(check_series[i]);
            if (!value) {
                return false;
            }
            worker.rate_limit_checks_[i] = *value;
        }
        for (std::size_t i = 0; i < settle_series.size(); ++i) {
            auto value = shard->counter(settle_series[i]);
            if (!value) {
                return false;
            }
            worker.rate_limit_settles_[i] = *value;
        }
    }
    return true;
}

AiServerMetrics::Worker &AiServerMetrics::worker(std::size_t index) noexcept {
    FIBER_ASSERT(valid_);
    FIBER_ASSERT(index < workers_.size());
    return workers_[index];
}

void AiServerMetrics::record_token_usage(Worker &worker, std::string_view username, std::string_view provider_name,
                                         LlmWireProtocol protocol, const LlmTokenUsage &usage) noexcept {
    FIBER_ASSERT(worker.owner_ == this);
    FIBER_ASSERT(worker.token_usage_cache_ != nullptr);
    token_usage_store_->record(*worker.token_usage_cache_, username, provider_name, protocol, usage);
}

void AiServerMetrics::set_config_generation(std::uint64_t generation) noexcept {
    std::uint64_t current = config_generation_.load(std::memory_order_relaxed);
    while (current < generation && !config_generation_.compare_exchange_weak(
                                           current, generation, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

async::Task<common::IoResult<mem::IoBufChain>>
AiServerMetrics::collect(mem::IoBufNodePool &node_pool, TokenRateLimiterStats limiter_stats, std::size_t cluster_nodes,
                         const log::AppenderStats *audit_stats) noexcept {
    auto collected = co_await registry_.collect_text(node_pool, prometheus::CollectOptions{
                                                                        .max_output_bytes = kMaxMetricsOutputBytes,
                                                                });
    if (!collected) {
        co_return std::unexpected(collected.error());
    }
    BoundedTextBuilder output(*collected, kMaxMetricsOutputBytes);
    if (!append_gauge(output, "ai_server_config_generation", "Latest installed configuration generation.",
                      config_generation_.load(std::memory_order_acquire)) ||
        !append_gauge(output, "ai_server_rate_limit_entries", "Local token rate limiter entries.",
                      limiter_stats.limiter_count) ||
        !append_gauge(output, "ai_server_rate_limit_inflight", "Local token rate limiter in-flight sessions.",
                      limiter_stats.in_flight_count) ||
        !append_gauge(output, "ai_server_rate_limit_cluster_nodes", "Token rate limit shard ring nodes.",
                      cluster_nodes) ||
        !append_process_metrics(output) || !token_usage_store_->append_text(output)) {
        co_return std::unexpected(output.error());
    }
    if (audit_stats && (!append_counter(output, "ai_server_audit_written_records_total", "LLM audit records written.",
                                        audit_stats->written_records) ||
                        !append_counter(output, "ai_server_audit_written_bytes_total", "LLM audit bytes written.",
                                        audit_stats->written_bytes) ||
                        !append_counter(output, "ai_server_audit_dropped_records_total", "LLM audit records dropped.",
                                        audit_stats->dropped_records) ||
                        !append_counter(output, "ai_server_audit_write_failures_total", "LLM audit write failures.",
                                        audit_stats->write_errors) ||
                        !append_counter(output, "ai_server_audit_rotations_total", "LLM audit log rotations.",
                                        audit_stats->rotations) ||
                        !append_counter(output, "ai_server_audit_rotation_failures_total",
                                        "LLM audit rotation failures.", audit_stats->rotation_errors) ||
                        !append_counter(output, "ai_server_audit_reopen_failures_total", "LLM audit reopen failures.",
                                        audit_stats->reopen_errors) ||
                        !append_counter(output, "ai_server_audit_retention_failures_total",
                                        "LLM audit retention failures.", audit_stats->retention_errors) ||
                        !append_gauge(output, "ai_server_audit_active_file_bytes", "Current LLM audit file size.",
                                      audit_stats->active_file_bytes))) {
        co_return std::unexpected(output.error());
    }
    co_return std::move(*collected);
}

void AiServerMetrics::stop_collecting() noexcept {
    if (!valid_ || collecting_stopped_) {
        return;
    }
    registry_.stop_collecting();
    collecting_stopped_ = true;
}

async::Task<void> AiServerMetrics::wait_for_idle() noexcept {
    if (valid_) {
        co_await registry_.wait_for_idle();
    }
}

} // namespace fiber::ai_server
