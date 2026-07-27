#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <netinet/in.h>
#include <openssl/hmac.h>
#include <sys/socket.h>
#include <unistd.h>

#include <async/Spawn.h>
#include <common/Assert.h>
#include <common/IoError.h>
#include <common/json/JsonParse.h>
#include <common/json/JsonStructDecode.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <http/ClientHttp1Exchange.h>
#include <http/ClientHttp1Types.h>
#include <http/Http1ClientConnection.h>
#include <http/Http1Server.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <net/SocketAddress.h>

#include "config/LlmConfigSnapshot.h"
#include "limit/RateLimitShardRing.h"
#include "limit/TokenRateLimitCoordinator.h"
#include "limit/TokenRateLimitRemoteClient.h"
#include "limit/TokenRateLimitService.h"
#include "observability/AiServerMetrics.h"
#include "provider/ProviderConnectionManager.h"
#include "provider/ProviderHttpClient.h"
#include "provider/ProviderRuntime.h"
#include "server/LlmRequestHandler.h"
#include "server/TokenRateLimitHttpHandler.h"

namespace {

using namespace std::chrono_literals;
using fiber::ai_server::AiServerMetrics;
using fiber::ai_server::Bt1Key;
using fiber::ai_server::Bt1KeySnapshot;
using fiber::ai_server::CompiledModelRateLimitRule;
using fiber::ai_server::CompiledModelRoute;
using fiber::ai_server::ConfigMetadata;
using fiber::ai_server::DiscoveredInstance;
using fiber::ai_server::DiscoveredService;
using fiber::ai_server::InstanceReportOutcome;
using fiber::ai_server::LlmConfigSnapshot;
using fiber::ai_server::LlmProjectSnapshot;
using fiber::ai_server::LlmRequestHandler;
using fiber::ai_server::LlmWireProtocol;
using fiber::ai_server::LoadBalancer;
using fiber::ai_server::ProjectProvider;
using fiber::ai_server::ProviderApiToken;
using fiber::ai_server::ProviderConfigSnapshot;
using fiber::ai_server::ProviderProtocol;
using fiber::ai_server::ProviderProtocolType;
using fiber::ai_server::ServiceInstancePolicy;

constexpr std::string_view kBt1Kid = "test1";
constexpr std::string_view kBt1Secret = "integration-test-secret";
constexpr std::string_view kRandomBytes = "abcdefghijklmnop";

struct MockReply {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::string retry_after;
    std::vector<std::string> chunks;
    bool stream = false;
    bool abort_before_header = false;
    bool abort_after_chunks = false;
};

struct ObservedProviderRequest {
    std::string path;
    std::string authorization;
    std::string body;
};

struct RawHttpResponse {
    int status = 0;
    std::string body;
    bool complete = false;
    int system_error = 0;
};

struct FixturePorts {
    std::uint16_t entry = 0;
    std::uint16_t provider = 0;
};

class StderrCapture {
public:
    ~StderrCapture() { finish(); }

    [[nodiscard]] bool start() noexcept {
        int descriptors[2];
        if (::pipe(descriptors) != 0) {
            return false;
        }
        saved_stderr_ = ::dup(STDERR_FILENO);
        if (saved_stderr_ < 0 || ::dup2(descriptors[1], STDERR_FILENO) < 0) {
            if (saved_stderr_ >= 0) {
                ::close(saved_stderr_);
                saved_stderr_ = -1;
            }
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            return false;
        }
        ::close(descriptors[1]);
        read_fd_ = descriptors[0];
        active_ = true;
        reader_ = std::thread([this]() {
            char buffer[8192];
            for (;;) {
                const ssize_t size = ::read(read_fd_, buffer, sizeof(buffer));
                if (size > 0) {
                    output_.append(buffer, static_cast<std::size_t>(size));
                    continue;
                }
                if (size < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            ::close(read_fd_);
            read_fd_ = -1;
        });
        return true;
    }

    void finish() noexcept {
        if (!active_) {
            return;
        }
        (void) ::dup2(saved_stderr_, STDERR_FILENO);
        ::close(saved_stderr_);
        saved_stderr_ = -1;
        if (reader_.joinable()) {
            reader_.join();
        }
        active_ = false;
    }

    [[nodiscard]] const std::string &output() const noexcept { return output_; }

private:
    std::thread reader_;
    std::string output_;
    int saved_stderr_ = -1;
    int read_fd_ = -1;
    bool active_ = false;
};

fiber::common::IoResult<std::uint16_t> bound_port(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return address.port();
}

std::string consume_chain(fiber::mem::IoBufChain chain) {
    std::string output;
    while (const fiber::mem::IoBuf *part = chain.first_readable()) {
        output.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
        chain.consume_and_compact(part->readable());
    }
    return output;
}

fiber::async::Task<fiber::common::IoResult<std::string>> read_body(fiber::http::HttpExchange &exchange) noexcept {
    std::string output;
    for (;;) {
        auto chunk = co_await exchange.read_body(64 * 1024);
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        const bool complete = chunk->complete();
        output.append(consume_chain(std::move(*chunk)));
        if (complete) {
            break;
        }
    }
    co_return output;
}

std::string base64url_encode(std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((bytes.size() * 4 + 2) / 3);
    std::size_t pos = 0;
    while (pos + 3 <= bytes.size()) {
        const auto a = static_cast<std::uint8_t>(bytes[pos]);
        const auto b = static_cast<std::uint8_t>(bytes[pos + 1]);
        const auto c = static_cast<std::uint8_t>(bytes[pos + 2]);
        output.push_back(alphabet[a >> 2]);
        output.push_back(alphabet[((a & 0x03U) << 4) | (b >> 4)]);
        output.push_back(alphabet[((b & 0x0fU) << 2) | (c >> 6)]);
        output.push_back(alphabet[c & 0x3fU]);
        pos += 3;
    }
    if (pos < bytes.size()) {
        const auto a = static_cast<std::uint8_t>(bytes[pos]);
        output.push_back(alphabet[a >> 2]);
        if (pos + 1 == bytes.size()) {
            output.push_back(alphabet[(a & 0x03U) << 4]);
        } else {
            const auto b = static_cast<std::uint8_t>(bytes[pos + 1]);
            output.push_back(alphabet[((a & 0x03U) << 4) | (b >> 4)]);
            output.push_back(alphabet[(b & 0x0fU) << 2]);
        }
    }
    return output;
}

std::string issue_token() {
    const auto expires =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count() +
            3600;
    std::string signing_input = "BT1.";
    signing_input.append(kBt1Kid);
    signing_input.push_back('.');
    signing_input.append(base64url_encode("alice"));
    signing_input.push_back('.');
    signing_input.append(std::to_string(expires));
    signing_input.push_back('.');
    signing_input.append(base64url_encode(kRandomBytes));

    std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (!HMAC(EVP_sha256(), kBt1Secret.data(), kBt1Secret.size(),
              reinterpret_cast<const std::uint8_t *>(signing_input.data()), signing_input.size(), digest.data(),
              &digest_size)) {
        return {};
    }
    signing_input.push_back('.');
    signing_input.append(
            base64url_encode({reinterpret_cast<const char *>(digest.data()), static_cast<std::size_t>(digest_size)}));
    return signing_input;
}

RawHttpResponse post_json(std::uint16_t port, std::string_view token, std::string_view body) {
    fiber::event::EventLoop loop;
    std::promise<RawHttpResponse> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop,
                        [&loop, port, token = std::string(token), body = std::string(body),
                         &promise]() mutable -> fiber::async::DetachedTask {
                            RawHttpResponse response;
                            fiber::http::Http1ClientConnectionOptions options;
                            options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
                            fiber::http::Http1ClientConnection connection(loop, std::move(options));
                            auto connected = co_await connection.connect(5s);
                            if (!connected) {
                                response.system_error = static_cast<int>(connected.error());
                                promise.set_value(std::move(response));
                                loop.stop();
                                co_return;
                            }

                            fiber::mem::BufPool pool;
                            fiber::http::HttpHeaders headers(pool);
                            headers.set_view("Host", "127.0.0.1");
                            headers.set_view("Content-Type", "application/json");
                            std::string authorization = "Bearer ";
                            authorization.append(token);
                            headers.set("Authorization", authorization);
                            fiber::http::ClientHttp1Exchange exchange(connection, pool);
                            auto sent_header = co_await exchange.send_header(
                                    fiber::http::Http1RequestHead{
                                            .method = fiber::http::HttpMethod::Post,
                                            .target = "/v1/chat/completions",
                                            .headers = &headers,
                                            .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
                                    },
                                    body.empty(), 5s);
                            if (!sent_header) {
                                response.system_error = static_cast<int>(sent_header.error());
                                promise.set_value(std::move(response));
                                loop.stop();
                                co_return;
                            }
                            if (!body.empty()) {
                                auto written = co_await exchange.write_body(
                                        reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true, 5s);
                                if (!written) {
                                    response.system_error = static_cast<int>(written.error());
                                    promise.set_value(std::move(response));
                                    loop.stop();
                                    co_return;
                                }
                            }

                            const fiber::http::Http1ResponseHead *head = nullptr;
                            for (;;) {
                                auto received = co_await exchange.read_header(5s);
                                if (!received) {
                                    response.system_error = static_cast<int>(received.error());
                                    promise.set_value(std::move(response));
                                    loop.stop();
                                    co_return;
                                }
                                if (!(*received)->is_informational()) {
                                    head = *received;
                                    break;
                                }
                            }
                            response.status = head->status_code;
                            for (;;) {
                                auto chunk = co_await exchange.read_body(64 * 1024, 5s);
                                if (!chunk) {
                                    response.system_error = static_cast<int>(chunk.error());
                                    break;
                                }
                                const bool complete = chunk->complete();
                                response.body.append(consume_chain(std::move(*chunk)));
                                if (complete) {
                                    response.complete = true;
                                    break;
                                }
                            }
                            promise.set_value(std::move(response));
                            loop.stop();
                        });
    loop.run();
    return future.get();
}

std::string bearer_token_name(std::string_view authorization) {
    constexpr std::string_view prefix = "Bearer ";
    return authorization.starts_with(prefix) ? std::string(authorization.substr(prefix.size())) : std::string{};
}

class ProxyFixture {
public:
    ProxyFixture(fiber::event::EventLoopGroup &group, std::vector<MockReply> replies, bool fail_settle,
                 bool service_rendezvous) :
        group_(&group), replies_(std::move(replies)), fail_settle_(fail_settle),
        service_rendezvous_(service_rendezvous), rate_limiters_(1), rate_limit_handler_(rate_limiters_),
        remote_client_(group), coordinator_(rate_limiters_, ring_, remote_client_), connections_(group),
        provider_client_(connections_), metrics_(group),
        provider_server_(group.at(0),
                         [this](fiber::http::HttpExchange &exchange) { return handle_provider(exchange); }),
        failing_provider_server_(
                group.at(0), [this](fiber::http::HttpExchange &exchange) { return handle_failing_provider(exchange); }),
        entry_server_(group.at(0), [this](fiber::http::HttpExchange &exchange) { return handle_entry(exchange); }) {}

    ~ProxyFixture() = default;

    fiber::async::DetachedTask start(std::promise<FixturePorts> *promise) noexcept {
        FixturePorts ports;
        if (!metrics_.valid() || !coordinator_.init() || !co_await connections_.init()) {
            promise->set_value(ports);
            co_return;
        }
        initialized_ = true;

        const fiber::net::SocketAddress address(fiber::net::IpAddress::loopback_v4(), 0);
        if (!provider_server_.bind(address, {})) {
            promise->set_value(ports);
            co_return;
        }
        auto provider_port = bound_port(provider_server_.fd());
        if (!provider_port) {
            promise->set_value(ports);
            co_return;
        }
        ports.provider = *provider_port;
        std::uint16_t failing_provider_port = 0;
        if (service_rendezvous_) {
            if (!failing_provider_server_.bind(address, {})) {
                promise->set_value(ports);
                co_return;
            }
            auto failed_port = bound_port(failing_provider_server_.fd());
            if (!failed_port) {
                promise->set_value(ports);
                co_return;
            }
            failing_provider_port = *failed_port;
        }
        config_ = make_config(*provider_port, failing_provider_port);
        runtime_.reconcile(*config_->project);

        if (!entry_server_.bind(address, {})) {
            promise->set_value(ports);
            co_return;
        }
        auto entry_port = bound_port(entry_server_.fd());
        if (!entry_port) {
            promise->set_value(ports);
            co_return;
        }
        ports.entry = *entry_port;
        if (!ring_.update(1, {
                                     {
                                             .node_id = "remote-owner",
                                             .host = "127.0.0.1",
                                             .port = ports.entry,
                                     },
                             })) {
            promise->set_value({});
            co_return;
        }
        fiber::async::spawn([this]() { return provider_server_.serve(); });
        if (service_rendezvous_) {
            fiber::async::spawn([this]() { return failing_provider_server_.serve(); });
        }
        fiber::async::spawn([this]() { return entry_server_.serve(); });
        promise->set_value(ports);
    }

    fiber::async::DetachedTask shutdown(std::promise<void> *promise) noexcept {
        provider_server_.close();
        if (service_rendezvous_) {
            failing_provider_server_.close();
        }
        if (initialized_) {
            co_await coordinator_.shutdown();
            co_await connections_.shutdown();
            initialized_ = false;
        }
        entry_server_.close();
        co_await entry_server_.shutdown_and_wait();
        co_await provider_server_.shutdown_and_wait();
        if (service_rendezvous_) {
            co_await failing_provider_server_.shutdown_and_wait();
        }
        metrics_.stop_collecting();
        co_await metrics_.wait_for_idle();
        promise->set_value();
    }

    fiber::async::DetachedTask token_available(std::string token_name, std::chrono::seconds after,
                                               std::promise<bool> *promise) noexcept {
        const bool available = runtime_.state_for("primary").token_available(
                token_name, fiber::event::EventLoop::current().now() + after);
        promise->set_value(available);
        co_return;
    }

    fiber::async::DetachedTask
    collect_settlement_metrics(std::promise<fiber::common::IoResult<std::string>> *promise) noexcept {
        co_await coordinator_.shutdown();
        auto collected = co_await metrics_.collect(fiber::event::EventLoop::current().io_buf_node_pool(),
                                                   rate_limiters_.stats(), 1);
        if (!collected) {
            promise->set_value(std::unexpected(collected.error()));
        } else {
            promise->set_value(consume_chain(std::move(*collected)));
        }
    }

    std::vector<ObservedProviderRequest> observed() const {
        std::lock_guard lock(mutex_);
        return observed_;
    }

    [[nodiscard]] int entry_calls() const noexcept { return entry_calls_.load(std::memory_order_acquire); }
    [[nodiscard]] int completed_entry_calls() const noexcept {
        return completed_entry_calls_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int failing_provider_calls() const noexcept {
        return failing_provider_calls_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string_view rendezvous_route_key() const noexcept { return rendezvous_route_key_; }

private:
    static DiscoveredInstance make_discovered_instance(std::string id, std::uint16_t port) {
        const fiber::net::IpAddress ip = fiber::net::IpAddress::loopback_v4();
        return DiscoveredInstance{
                .instance_id = std::move(id),
                .address = fiber::net::SocketAddress(ip, port),
                .connection_key = fiber::http::Http1ConnectionGroupKey::from_ip(
                        ip, port, fiber::http::Http1ConnectionGroupKey::Scheme::Http),
                .host_header = "127.0.0.1:" + std::to_string(port),
                .weight = 1.0,
                .cluster_name = "primary",
        };
    }

    std::shared_ptr<const LlmConfigSnapshot> make_config(std::uint16_t provider_port,
                                                         std::uint16_t failing_provider_port) {
        auto keys = std::make_shared<Bt1KeySnapshot>();
        keys->keys.push_back(Bt1Key{
                .kid = std::string(kBt1Kid),
                .secret = std::string(kBt1Secret),
        });

        const std::string direct_base_url = "http://127.0.0.1:" + std::to_string(provider_port);
        auto primary_config = std::make_shared<ProviderConfigSnapshot>();
        primary_config->metadata.version = 11;
        primary_config->name = "primary";
        primary_config->base_url = service_rendezvous_ ? "service://mock-provider" : direct_base_url;
        primary_config->api_tokens = {
                {.name = "token-a", .token = "token-a"},
                {.name = "token-b", .token = "token-b"},
                {.name = "token-c", .token = "token-c"},
        };
        primary_config->protocols.push_back(ProviderProtocol{
                .type = ProviderProtocolType::OpenAiChatCompletions,
                .path = "/provider/chat",
                .model = "upstream-primary",
        });

        auto fallback_config = std::make_shared<ProviderConfigSnapshot>();
        fallback_config->metadata.version = 12;
        fallback_config->name = "fallback";
        fallback_config->base_url = direct_base_url;
        fallback_config->api_tokens = {
                {.name = "token-f", .token = "token-f"},
        };
        fallback_config->protocols.push_back(ProviderProtocol{
                .type = ProviderProtocolType::OpenAiChatCompletions,
                .path = "/provider/chat",
                .model = "upstream-fallback",
        });

        auto primary = std::make_shared<ProjectProvider>();
        primary->name = "primary";
        primary->config = std::move(primary_config);
        if (service_rendezvous_) {
            primary->service = std::make_shared<LoadBalancer>();
            (void) primary->service->update_instances(DiscoveredService{
                    .service_name = "mock-provider",
                    .group = "DEFAULT_GROUP",
                    .checksum = "service-v1",
                    .instances =
                            {
                                    make_discovered_instance("healthy", provider_port),
                                    make_discovered_instance("failing", failing_provider_port),
                            },
            });
            for (std::size_t i = 0; i < 1024; ++i) {
                std::string candidate = "service-route-" + std::to_string(i);
                const std::uint64_t key = fiber::ai_server::rendezvous_score(candidate, primary->name);
                auto selected = primary->service->load_balance(key, {}, LoadBalancer::TimePoint{});
                FIBER_ASSERT(selected.has_value());
                const bool selects_failing = selected->address().port() == failing_provider_port;
                primary->service->report(std::move(*selected), InstanceReportOutcome::Neutral,
                                         LoadBalancer::TimePoint{});
                if (selects_failing) {
                    rendezvous_route_key_ = std::move(candidate);
                    break;
                }
            }
            FIBER_ASSERT(!rendezvous_route_key_.empty());
        }
        auto fallback = std::make_shared<ProjectProvider>();
        fallback->name = "fallback";
        fallback->config = std::move(fallback_config);

        CompiledModelRoute route;
        route.model_name = "logical";
        route.providers.push_back(primary);
        route.fallback_provider = fallback;
        route.load_balance.max_primary_attempts = 1;
        route.load_balance.fallback_enabled = true;
        if (service_rendezvous_) {
            route.load_balance.service_instance_policy = ServiceInstancePolicy::WeightedRendezvous;
        }
        route.rate_limit = CompiledModelRateLimitRule{
                .revision = 17,
                .window_duration_millis = 60'000,
                .max_tokens_per_window = 1'000,
        };

        std::vector<std::shared_ptr<const ProjectProvider>> providers{primary, fallback};
        std::vector<CompiledModelRoute> routes;
        routes.push_back(std::move(route));
        auto project =
                std::make_shared<LlmProjectSnapshot>(ConfigMetadata{}, 1, std::move(providers), std::move(routes));

        auto snapshot = std::make_shared<LlmConfigSnapshot>();
        snapshot->generation = 1;
        snapshot->bt1_keys = std::move(keys);
        snapshot->project = std::move(project);
        return snapshot;
    }

    fiber::async::Task<void> handle_provider(fiber::http::HttpExchange &exchange) noexcept {
        auto body = co_await read_body(exchange);
        if (!body) {
            co_return;
        }

        MockReply reply;
        {
            std::lock_guard lock(mutex_);
            observed_.push_back(ObservedProviderRequest{
                    .path = std::string(exchange.uri().path),
                    .authorization = std::string(exchange.request_headers().get("authorization")),
                    .body = std::move(*body),
            });
            if (reply_index_ < replies_.size()) {
                reply = replies_[reply_index_++];
            } else {
                reply = MockReply{
                        .status = 500,
                        .body = R"({"error":{"code":"unexpected_call"}})",
                };
            }
        }
        if (reply.abort_before_header) {
            (void) exchange.abort(fiber::common::IoErr::Canceled);
            co_return;
        }

        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("Content-Type", reply.content_type);
        if (!reply.retry_after.empty()) {
            headers.set("Retry-After", reply.retry_after);
        }
        const std::size_t body_size = reply.stream ? 0 : reply.body.size();
        auto sent = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = reply.status,
                .headers = &headers,
                .body = reply.stream ? fiber::http::HttpBodySpec::Chunked()
                                     : fiber::http::HttpBodySpec::ContentLength(body_size),
                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                .end_stream = !reply.stream && body_size == 0,
        });
        if (!sent) {
            co_return;
        }
        if (!reply.stream) {
            if (body_size != 0) {
                (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(reply.body.data()),
                                                    reply.body.size(), true);
            }
            co_return;
        }

        for (std::size_t i = 0; i < reply.chunks.size(); ++i) {
            const bool end = i + 1 == reply.chunks.size() && !reply.abort_after_chunks;
            const std::string &chunk = reply.chunks[i];
            auto written = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(chunk.data()),
                                                        chunk.size(), end);
            if (!written) {
                co_return;
            }
        }
        if (reply.abort_after_chunks) {
            (void) exchange.abort(fiber::common::IoErr::Canceled);
        } else if (reply.chunks.empty()) {
            (void) co_await exchange.write_body(nullptr, 0, true);
        }
    }

    fiber::async::Task<void> handle_failing_provider(fiber::http::HttpExchange &exchange) noexcept {
        failing_provider_calls_.fetch_add(1, std::memory_order_release);
        (void) exchange.abort(fiber::common::IoErr::Canceled);
        co_return;
    }

    fiber::async::Task<void> handle_entry(fiber::http::HttpExchange &exchange) noexcept {
        if (exchange.uri().path == "/internal/llm/rate-limit/check") {
            co_await rate_limit_handler_.handle_check(exchange);
            co_return;
        }
        if (exchange.uri().path == "/internal/llm/rate-limit/settle") {
            if (fail_settle_) {
                (void) exchange.abort(fiber::common::IoErr::Canceled);
                co_return;
            }
            co_await rate_limit_handler_.handle_settle(exchange);
            co_return;
        }
        entry_calls_.fetch_add(1, std::memory_order_release);
        AiServerMetrics::Worker &metrics = metrics_.worker(0);
        metrics.request_started(LlmWireProtocol::OpenAiChatCompletions);
        const auto started = fiber::event::EventLoop::current().now();
        LlmRequestHandler handler(provider_client_, runtime_, coordinator_, metrics, nullptr);
        co_await handler.handle(exchange, LlmWireProtocol::OpenAiChatCompletions, config_);
        metrics.request_finished(LlmWireProtocol::OpenAiChatCompletions, exchange.response_stats(),
                                 std::chrono::duration_cast<std::chrono::microseconds>(
                                         fiber::event::EventLoop::current().now() - started));
        completed_entry_calls_.fetch_add(1, std::memory_order_release);
    }

    fiber::event::EventLoopGroup *group_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<MockReply> replies_;
    std::vector<ObservedProviderRequest> observed_;
    std::size_t reply_index_ = 0;
    std::atomic<int> entry_calls_{0};
    std::atomic<int> completed_entry_calls_{0};
    std::atomic<int> failing_provider_calls_{0};
    bool fail_settle_ = false;
    bool service_rendezvous_ = false;
    std::string rendezvous_route_key_;
    fiber::ai_server::TokenRateLimitService rate_limiters_;
    fiber::ai_server::TokenRateLimitHttpHandler rate_limit_handler_;
    fiber::ai_server::RateLimitShardRing ring_;
    fiber::ai_server::TokenRateLimitRemoteClient remote_client_;
    fiber::ai_server::TokenRateLimitCoordinator coordinator_;
    fiber::ai_server::ProviderConnectionManager connections_;
    fiber::ai_server::ProviderHttpClient provider_client_;
    AiServerMetrics metrics_;
    fiber::ai_server::ProviderRuntimeRegistry runtime_;
    std::shared_ptr<const LlmConfigSnapshot> config_;
    fiber::http::Http1Server provider_server_;
    fiber::http::Http1Server failing_provider_server_;
    fiber::http::Http1Server entry_server_;
    bool initialized_ = false;
};

class FixtureHarness {
public:
    explicit FixtureHarness(std::vector<MockReply> replies, bool fail_settle = false, bool service_rendezvous = false) :
        fixture_(std::make_unique<ProxyFixture>(group_, std::move(replies), fail_settle, service_rendezvous)) {
        group_.start();
        std::promise<FixturePorts> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, &promise]() { return fixture_->start(&promise); });
        if (future.wait_for(5s) == std::future_status::ready) {
            ports_ = future.get();
        }
    }

    ~FixtureHarness() {
        std::promise<void> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, &promise]() { return fixture_->shutdown(&promise); });
        (void) future.wait_for(5s);
        group_.stop();
        group_.join();
        fixture_.reset();
    }

    [[nodiscard]] bool valid() const noexcept { return ports_.entry != 0 && ports_.provider != 0; }

    [[nodiscard]] std::uint16_t entry_port() const noexcept { return ports_.entry; }

    [[nodiscard]] std::vector<ObservedProviderRequest> observed() const { return fixture_->observed(); }

    [[nodiscard]] int entry_calls() const noexcept { return fixture_->entry_calls(); }
    [[nodiscard]] int completed_entry_calls() const noexcept { return fixture_->completed_entry_calls(); }
    [[nodiscard]] int failing_provider_calls() const noexcept { return fixture_->failing_provider_calls(); }
    [[nodiscard]] std::string rendezvous_route_key() const { return std::string(fixture_->rendezvous_route_key()); }

    bool token_available(std::string token_name, std::chrono::seconds after) {
        std::promise<bool> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, token_name = std::move(token_name), after, &promise]() mutable {
            return fixture_->token_available(std::move(token_name), after, &promise);
        });
        return future.wait_for(5s) == std::future_status::ready && future.get();
    }

    fiber::common::IoResult<std::string> settlement_metrics() {
        std::promise<fiber::common::IoResult<std::string>> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0),
                            [this, &promise]() { return fixture_->collect_settlement_metrics(&promise); });
        if (future.wait_for(5s) != std::future_status::ready) {
            return std::unexpected(fiber::common::IoErr::TimedOut);
        }
        return future.get();
    }

private:
    fiber::event::EventLoopGroup group_{1};
    std::unique_ptr<ProxyFixture> fixture_;
    FixturePorts ports_;
};

TEST(LlmProxyIntegrationTest, RetriesTransportAuthRateLimitAndFallbackFromOriginalJson) {
    FixtureHarness fixture({
            MockReply{.abort_before_header = true},
            MockReply{
                    .status = 401,
                    .body = R"({"error":{"type":"authentication_error","code":"invalid_api_key"}})",
            },
            MockReply{
                    .status = 429,
                    .body = R"({"error":{"type":"rate_limit_error","code":"rate_limit_exceeded"}})",
                    .retry_after = "120",
            },
            MockReply{
                    .status = 200,
                    .body = R"({"id":"ok","usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})",
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({ "model" : "logical", "stream":false, "unknown":1.2300, "nested":{"x":"y"} })";

    const RawHttpResponse response = post_json(fixture.entry_port(), token, request);

    EXPECT_EQ(response.system_error, 0);
    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.complete);
    EXPECT_EQ(response.body, R"({"id":"ok","usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})");
    const auto observed = fixture.observed();
    EXPECT_EQ(fixture.entry_calls(), 1);
    ASSERT_EQ(observed.size(), 4u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NE(observed[i].body.find(R"("model" : "upstream-primary")"), std::string::npos);
        EXPECT_NE(observed[i].body.find(R"("unknown":1.2300)"), std::string::npos);
    }
    EXPECT_NE(observed[3].body.find(R"("model" : "upstream-fallback")"), std::string::npos);
    EXPECT_NE(observed[3].body.find(R"("unknown":1.2300)"), std::string::npos);

    const std::string auth_failed = bearer_token_name(observed[1].authorization);
    const std::string rate_limited = bearer_token_name(observed[2].authorization);
    EXPECT_FALSE(auth_failed.empty());
    EXPECT_FALSE(rate_limited.empty());
    EXPECT_NE(auth_failed, rate_limited);
    EXPECT_FALSE(fixture.token_available(auth_failed, 121s));
    EXPECT_TRUE(fixture.token_available(rate_limited, 121s));
}

TEST(LlmProxyIntegrationTest, WeightedRendezvousRetryExcludesFailedServiceInstance) {
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"ok","usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})",
                    },
            },
            false, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    const std::string route_key = fixture.rendezvous_route_key();
    ASSERT_FALSE(route_key.empty());
    const std::string request = R"({"model":"logical","stream":false,"metadata":{"route_key":")" + route_key +
                                R"("},"messages":[{"role":"user","content":"hello"}]})";

    const RawHttpResponse response = post_json(fixture.entry_port(), token, request);

    EXPECT_EQ(response.system_error, 0);
    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.complete);
    EXPECT_EQ(fixture.failing_provider_calls(), 1);
    const auto observed = fixture.observed();
    ASSERT_EQ(observed.size(), 1u);
    EXPECT_NE(observed[0].body.find(R"("model":"upstream-primary")"), std::string::npos);
}

TEST(LlmProxyIntegrationTest, RelaysSseBytesUnchangedAndNeverRetriesAfterResponseStart) {
    constexpr std::string_view expected =
            ": ping\r\nid: 7\r\ndata:{\"choices\":[],\"usage\":{\"prompt_tokens\":2"
            ",\"completion_tokens\":3,\"total_tokens\":5}}\r\n\r\ndata: [DONE]\r\n\r\ndata: [DONE]\n\n";
    FixtureHarness fixture({
            MockReply{
                    .status = 200,
                    .content_type = "text/event-stream",
                    .chunks =
                            {
                                    ": ping\r\nid: 7\r\ndata:{\"choices\":[],\"usage\":{\"prompt_tokens\":2",
                                    ",\"completion_tokens\":3,\"total_tokens\":5}}\r\n\r\ndata: [DONE]\r\n\r\n"
                                    "data: [DONE]\n\n",
                            },
                    .stream = true,
            },
            MockReply{
                    .status = 200,
                    .content_type = "text/event-stream",
                    .chunks =
                            {
                                    "data: {\"choices\":[]}\n\n",
                            },
                    .stream = true,
                    .abort_after_chunks = true,
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({"model":"logical","stream":true,"messages":[{"role":"user","content":"hello"}]})";

    const RawHttpResponse success = post_json(fixture.entry_port(), token, request);
    EXPECT_EQ(success.status, 200);
    EXPECT_TRUE(success.complete);
    EXPECT_EQ(success.body, expected);

    const RawHttpResponse truncated = post_json(fixture.entry_port(), token, request);
    EXPECT_EQ(truncated.status, 200);
    EXPECT_TRUE(truncated.complete);
    EXPECT_EQ(truncated.body.find("data: [DONE]"), std::string::npos);
    EXPECT_EQ(fixture.observed().size(), 2u);
}

TEST(LlmProxyIntegrationTest, SettlementFailureDoesNotChangeProviderResponse) {
    constexpr std::string_view buffered_body =
            R"({"id":"ok","usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})";
    constexpr std::string_view sse_body =
            ": ping\r\ndata: {\"choices\":[],\"usage\":{\"prompt_tokens\":4,\"completion_tokens\":6,"
            "\"total_tokens\":10}}\r\n\r\n";
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .body = std::string(buffered_body),
                    },
                    MockReply{
                            .status = 200,
                            .content_type = "text/event-stream",
                            .chunks = {std::string(sse_body)},
                            .stream = true,
                    },
            },
            true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse buffered =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":false,"messages":[]})");
    EXPECT_EQ(buffered.system_error, 0);
    EXPECT_EQ(buffered.status, 200);
    EXPECT_TRUE(buffered.complete);
    EXPECT_EQ(buffered.body, buffered_body);

    const RawHttpResponse streamed =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":true,"messages":[]})");
    EXPECT_EQ(streamed.system_error, 0);
    EXPECT_EQ(streamed.status, 200);
    EXPECT_TRUE(streamed.complete);
    EXPECT_EQ(streamed.body, sse_body);

    auto metrics = fixture.settlement_metrics();
    ASSERT_TRUE(metrics);
    EXPECT_NE(metrics->find("ai_server_rate_limit_settlements_total{result=\"error\"} 2"), std::string::npos);
}

TEST(LlmProxyIntegrationTest, EmitsOneJsonAuditLineWithInputAndOutput) {
    FixtureHarness fixture({
            MockReply{
                    .status = 200,
                    .body = R"({"id":"ok","choices":[{"message":{"role":"assistant","content":"weather is sunny","tool_calls":[{"type":"function","function":{"name":"weather","arguments":"{\"city\":\"Paris\"}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":8,"completion_tokens":6,"total_tokens":14}})",
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({"model":"logical","stream":false,"messages":[{"role":"system","content":"answer briefly"},{"role":"user","content":[{"type":"text","text":"weather in Paris"},{"type":"image_url","image_url":{"url":"https://example.test/image?signature=SECRET_URL"}},{"type":"input_audio","input_audio":{"data":"SECRET_BASE64"}}]}],"tools":[{"type":"function","function":{"name":"weather","description":"look up weather","parameters":{"type":"object"}}}]})";
    StderrCapture capture;
    ASSERT_TRUE(capture.start());

    const RawHttpResponse response = post_json(fixture.entry_port(), token, request);
    for (std::size_t i = 0; i < 1000 && fixture.completed_entry_calls() == 0; ++i) {
        std::this_thread::sleep_for(1ms);
    }
    capture.finish();

    ASSERT_EQ(response.system_error, 0);
    ASSERT_EQ(response.status, 200);
    ASSERT_TRUE(response.complete);
    const std::string &logs = capture.output();
    constexpr std::string_view marker = R"("event":"llm_request")";
    const std::size_t marker_pos = logs.find(marker);
    ASSERT_NE(marker_pos, std::string::npos) << logs;
    EXPECT_EQ(logs.find(marker, marker_pos + marker.size()), std::string::npos);
    EXPECT_EQ(logs.find(R"("event":"provider_attempt")"), std::string::npos);

    const std::size_t line_begin = logs.rfind('\n', marker_pos);
    const std::size_t json_begin = logs.find('{', line_begin == std::string::npos ? 0 : line_begin + 1);
    const std::size_t line_end = logs.find('\n', marker_pos);
    ASSERT_NE(json_begin, std::string::npos);
    ASSERT_NE(line_end, std::string::npos);
    const std::string_view audit_json(logs.data() + json_begin, line_end - json_begin);
    EXPECT_EQ(audit_json.find('\n'), std::string_view::npos);

    fiber::mem::BufPool pool;
    fiber::json::JsonParser parser;
    ASSERT_TRUE(parser.feed(audit_json.data(), audit_json.size()));
    parser.finish();
    fiber::json::JsonAny root;
    ASSERT_EQ(fiber::json::parse_document(parser, pool, root,
                                          [](fiber::json::JsonParser &value_parser, fiber::mem::BufPool &value_pool,
                                             fiber::json::JsonAny &value) noexcept {
                                              return fiber::json::parse_any(value_parser, value_pool, value);
                                          }),
              fiber::json::ParseStatus::Done);
    ASSERT_TRUE(root.is_object());

    EXPECT_NE(audit_json.find("answer briefly"), std::string_view::npos);
    EXPECT_NE(audit_json.find("weather in Paris"), std::string_view::npos);
    EXPECT_NE(audit_json.find("look up weather"), std::string_view::npos);
    EXPECT_NE(audit_json.find("weather is sunny"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"(\"city\":\"Paris\")"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("provider_attempts":[{)"), std::string_view::npos);
    EXPECT_EQ(audit_json.find("SECRET_URL"), std::string_view::npos);
    EXPECT_EQ(audit_json.find("SECRET_BASE64"), std::string_view::npos);
}

} // namespace
