#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iterator>
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

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <common/Assert.h>
#include <common/IoError.h>
#include <common/json/JsonParse.h>
#include <common/json/JsonStructDecode.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <fiber/cat/Cat.h>
#include <http/ClientHttp1Exchange.h>
#include <http/ClientHttp1Types.h>
#include <http/Http1ClientConnection.h>
#include <http/Http1Server.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <log/LoggerManager.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>
#include <net/TcpStream.h>

#include "../../../tests/NacosSnapshotTestBuilder.h"
#include "AiServerLogging.h"
#include "config/LlmConfigSnapshot.h"
#include "limit/RateLimitShardRing.h"
#include "limit/TokenRateLimitCoordinator.h"
#include "limit/TokenRateLimitRemoteClient.h"
#include "limit/TokenRateLimitService.h"
#include "observability/AiServerCatRequest.h"
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
using fiber::ai_server::InstanceReportOutcome;
using fiber::ai_server::LlmConfigSnapshot;
using fiber::ai_server::LlmProjectSnapshot;
using fiber::ai_server::LlmRequestHandler;
using fiber::ai_server::LlmWireProtocol;
using fiber::ai_server::ProjectProvider;
using fiber::ai_server::ProviderApiToken;
using fiber::ai_server::ProviderConfigSnapshot;
using fiber::ai_server::ProviderProtocol;
using fiber::ai_server::ProviderProtocolType;
using fiber::ai_server::WeightedRendezvous;

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
    bool pause_before_header = false;
    std::size_t pause_after_chunks = 0;
};

struct ObservedProviderRequest {
    std::string path;
    std::string authorization;
    std::string trace_id;
    std::string parent_span_id;
    std::string span_id;
    std::string trace_state;
    std::string body;
};

struct ObservedRateLimitRequest {
    std::string path;
    std::string trace_id;
    std::string parent_span_id;
    std::string span_id;
    std::string trace_state;
};

struct ClientCatHeaders {
    std::string trace_id;
    std::string parent_span_id;
    std::string span_id;
    std::string trace_state;
    std::string user_agent;
    std::string real_ip;
};

struct RawHttpResponse {
    int status = 0;
    std::string trace_id;
    std::string body;
    bool complete = false;
    int system_error = 0;
};

struct FixturePorts {
    std::uint16_t entry = 0;
    std::uint16_t provider = 0;
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

int bind_dropping_dns_socket(std::uint16_t &port) noexcept {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        (void) ::close(fd);
        return -1;
    }
    auto resolved_port = bound_port(fd);
    if (!resolved_port) {
        (void) ::close(fd);
        return -1;
    }
    port = *resolved_port;
    return fd;
}

std::size_t drain_dns_packets(int fd) noexcept {
    std::size_t count = 0;
    std::array<std::uint8_t, 512> packet{};
    while (::recv(fd, packet.data(), packet.size(), MSG_DONTWAIT) >= 0) {
        ++count;
    }
    return count;
}

std::string consume_chain(fiber::mem::IoBufChain chain) {
    std::string output;
    while (const fiber::mem::IoBuf *part = chain.first_readable()) {
        output.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
        chain.consume_and_compact(part->readable());
    }
    return output;
}

std::string cat_nt1_type_and_name(std::string_view type, std::string_view name) {
    FIBER_ASSERT(type.size() < 128 && name.size() < 128);
    std::string encoded;
    encoded.reserve(type.size() + name.size() + 2);
    encoded.push_back(static_cast<char>(type.size()));
    encoded.append(type);
    encoded.push_back(static_cast<char>(name.size()));
    encoded.append(name);
    return encoded;
}

std::string cat_nt1_type_name_and_status(std::string_view type, std::string_view name, std::string_view status) {
    FIBER_ASSERT(status.size() < 128);
    std::string encoded = cat_nt1_type_and_name(type, name);
    encoded.push_back(static_cast<char>(status.size()));
    encoded.append(status);
    return encoded;
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

fiber::async::Task<fiber::common::IoResult<void>> read_exact_bytes(fiber::net::TcpStream &stream, std::uint8_t *data,
                                                                   std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        auto read = co_await stream.read(data + offset, size - offset, 30s);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read == 0) {
            co_return std::unexpected(fiber::common::IoErr::NotConnected);
        }
        offset += *read;
    }
    co_return fiber::common::IoResult<void>{};
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

std::string issue_token(std::string_view username = "alice") {
    const auto expires =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count() +
            3600;
    std::string signing_input = "BT1.";
    signing_input.append(kBt1Kid);
    signing_input.push_back('.');
    signing_input.append(base64url_encode(username));
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

RawHttpResponse post_json(std::uint16_t port, std::string_view token, std::string_view body,
                          ClientCatHeaders cat_headers = {}, std::string_view target = "/v1/chat/completions") {
    fiber::event::EventLoop loop;
    std::promise<RawHttpResponse> promise;
    auto future = promise.get_future();
    fiber::async::spawn(
            loop,
            [&loop, port, token = std::string(token), body = std::string(body), cat_headers = std::move(cat_headers),
             target = std::string(target), &promise]() mutable -> fiber::async::DetachedTask {
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
                if (!cat_headers.trace_id.empty()) {
                    headers.set("HI-TRACE-ID", cat_headers.trace_id);
                }
                if (!cat_headers.parent_span_id.empty()) {
                    headers.set("HI-SPAN-ID-PARENT", cat_headers.parent_span_id);
                }
                if (!cat_headers.span_id.empty()) {
                    headers.set("HI-SPAN-ID", cat_headers.span_id);
                }
                if (!cat_headers.trace_state.empty()) {
                    headers.set("tracestate", cat_headers.trace_state);
                }
                if (!cat_headers.user_agent.empty()) {
                    headers.set("User-Agent", cat_headers.user_agent);
                }
                if (!cat_headers.real_ip.empty()) {
                    headers.set("X-Real-Ip", cat_headers.real_ip);
                }
                fiber::http::ClientHttp1Exchange exchange(connection, pool);
                auto sent_header = co_await exchange.send_header(
                        fiber::http::Http1RequestHead{
                                .method = fiber::http::HttpMethod::Post,
                                .target = target,
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
                    auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()),
                                                               body.size(), true, 5s);
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
                response.trace_id = std::string(head->headers.get("hi-trace-id"));
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

int open_json_request(std::uint16_t port, std::string_view token, std::string_view body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval timeout{
            .tv_sec = 5,
            .tv_usec = 0,
    };
    (void) ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void) ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        (void) ::close(fd);
        return -1;
    }

    std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Content-Type: application/json\r\nAuthorization: Bearer ";
    request.append(token);
    request.append("\r\nContent-Length: ");
    request.append(std::to_string(body.size()));
    request.append("\r\n\r\n");
    request.append(body);
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t written = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            (void) ::close(fd);
            return -1;
        }
        sent += static_cast<std::size_t>(written);
    }
    return fd;
}

bool wait_for_socket_text(int fd, std::string_view expected, std::string &received) {
    std::array<char, 4096> buffer{};
    while (received.find(expected) == std::string::npos) {
        const ssize_t read = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (read <= 0) {
            return false;
        }
        received.append(buffer.data(), static_cast<std::size_t>(read));
    }
    return true;
}

void reset_http_client(int fd) noexcept {
    if (fd < 0) {
        return;
    }
    linger reset{
            .l_onoff = 1,
            .l_linger = 0,
    };
    (void) ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    (void) ::close(fd);
}

std::string bearer_token_name(std::string_view authorization) {
    constexpr std::string_view prefix = "Bearer ";
    return authorization.starts_with(prefix) ? std::string(authorization.substr(prefix.size())) : std::string{};
}

class ProxyFixture {
public:
    ProxyFixture(fiber::event::EventLoopGroup &group, std::vector<MockReply> replies, bool fail_settle,
                 bool service_rendezvous, std::size_t audit_max_record_bytes, bool enable_cat,
                 fiber::ai_server::WorkerDnsService::Options dns_options, bool primary_dns_timeout,
                 bool fallback_enabled) :
        group_(&group), replies_(std::move(replies)), fail_settle_(fail_settle),
        service_rendezvous_(service_rendezvous), audit_max_record_bytes_(audit_max_record_bytes),
        enable_cat_(enable_cat), primary_dns_timeout_(primary_dns_timeout), fallback_enabled_(fallback_enabled),
        rate_limiters_(1), remote_client_(group), coordinator_(rate_limiters_, ring_, remote_client_),
        connections_(group, std::move(dns_options)), provider_client_(connections_), metrics_(group),
        cat_collector_(group.at(0)),
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
        if (enable_cat_ && !start_cat()) {
            promise->set_value(ports);
            co_return;
        }

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
        release_provider();
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
        if (cat_client_) {
            (void) co_await cat_client_->detach_current_event_loop();
            co_await cat_client_->shutdown();
        }
        cat_collector_.close();
        co_await cat_collector_tasks_.join();
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

    std::vector<ObservedRateLimitRequest> observed_rate_limits() const {
        std::lock_guard lock(mutex_);
        return observed_rate_limits_;
    }

    [[nodiscard]] bool cat_frame_contains(std::string_view first, std::string_view second = {}) const {
        std::lock_guard lock(mutex_);
        for (const auto &frame: cat_frames_) {
            const bool contains_first =
                    std::search(frame.begin(), frame.end(), first.begin(), first.end()) != frame.end();
            const bool contains_second = second.empty() || std::search(frame.begin(), frame.end(), second.begin(),
                                                                       second.end()) != frame.end();
            if (contains_first && contains_second) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int entry_calls() const noexcept { return entry_calls_.load(std::memory_order_acquire); }
    [[nodiscard]] int completed_entry_calls() const noexcept {
        return completed_entry_calls_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int failing_provider_calls() const noexcept {
        return failing_provider_calls_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool provider_paused() const noexcept {
        return provider_pause_reached_.load(std::memory_order_acquire);
    }
    void release_provider() noexcept { provider_pause_released_.store(true, std::memory_order_release); }
    [[nodiscard]] std::string_view rendezvous_route_key() const noexcept { return rendezvous_route_key_; }

private:
    fiber::async::Task<void> pause_provider() noexcept {
        provider_pause_reached_.store(true, std::memory_order_release);
        while (!provider_pause_released_.load(std::memory_order_acquire)) {
            co_await fiber::async::sleep(1ms);
        }
    }

    fiber::async::DetachedTask collect_cat_frames() noexcept {
        auto accepted = co_await cat_collector_.accept();
        if (!accepted) {
            cat_collector_tasks_.done();
            co_return;
        }
        cat_collector_.close();
        fiber::net::TcpStream stream(fiber::event::EventLoop::current(), accepted->release_fd(), accepted->take_peer());
        for (;;) {
            std::array<std::uint8_t, 4> prefix{};
            auto prefix_result = co_await read_exact_bytes(stream, prefix.data(), prefix.size());
            if (!prefix_result) {
                break;
            }
            const std::size_t payload_size = static_cast<std::size_t>(prefix[0]) << 24U |
                                             static_cast<std::size_t>(prefix[1]) << 16U |
                                             static_cast<std::size_t>(prefix[2]) << 8U | prefix[3];
            if (payload_size == 0 || payload_size > 2 * 1024 * 1024) {
                break;
            }
            std::vector<std::uint8_t> frame(prefix.begin(), prefix.end());
            frame.resize(prefix.size() + payload_size);
            auto payload_result = co_await read_exact_bytes(stream, frame.data() + prefix.size(), payload_size);
            if (!payload_result) {
                break;
            }
            std::lock_guard lock(mutex_);
            cat_frames_.push_back(std::move(frame));
        }
        stream.close();
        cat_collector_tasks_.done();
    }

    bool start_cat() noexcept {
        auto bound = cat_collector_.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
        if (!bound) {
            return false;
        }
        auto collector_port = bound_port(cat_collector_.fd());
        if (!collector_port) {
            cat_collector_.close();
            return false;
        }
        cat_collector_tasks_.add();
        fiber::async::spawn([this]() { return collect_cat_frames(); });

        fiber::cat::CatClientConfigParams params{
                .app_key = "ai-server-test",
                .hostname = "test-host",
                .ip = "127.0.0.1",
                .thread_group_name = "test",
                .thread_id = "0",
                .thread_name = "worker",
        };
        params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), *collector_port);
        auto config = fiber::cat::CatClientConfig::create(std::move(params));
        if (!config) {
            return false;
        }
        fiber::cat::CatClientOptions options;
        options.enable_heartbeat = false;
        options.enable_system_stats = false;
        options.collector_connect_timeout = 10ms;
        options.collector_write_timeout = 10ms;
        options.reconnect_initial_delay = 10ms;
        options.reconnect_max_delay = 10ms;
        options.shutdown_drain_timeout = 20ms;
        options.aggregation_flush_interval = 10ms;
        auto created = fiber::cat::CatClient::create(group_->at(0), std::move(*config), options);
        if (!created) {
            return false;
        }
        cat_client_ = std::move(*created);
        const auto started = cat_client_->start();
        if (!started) {
            cat_client_.reset();
            return false;
        }
        return true;
    }

    static fiber::nacos::Instance make_service_instance(std::string id, std::uint16_t port) {
        return fiber::nacos::Instance{
                .instance_id = std::move(id),
                .ip = "127.0.0.1",
                .port = port,
                .weight = 1.0,
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
        primary_config->base_url =
                service_rendezvous_
                        ? "service://mock-provider"
                        : (primary_dns_timeout_ ? "http://dns-timeout.invalid:" + std::to_string(provider_port)
                                                : direct_base_url);
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
        primary_config->protocols.push_back(ProviderProtocol{
                .type = ProviderProtocolType::AnthropicMessages,
                .path = "/provider/messages",
                .model = "upstream-anthropic-primary",
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
        fallback_config->protocols.push_back(ProviderProtocol{
                .type = ProviderProtocolType::AnthropicMessages,
                .path = "/provider/messages",
                .model = "upstream-anthropic-fallback",
        });

        auto primary = std::make_shared<ProjectProvider>();
        primary->name = "primary";
        primary->config = std::move(primary_config);
        if (service_rendezvous_) {
            primary->service = std::make_shared<WeightedRendezvous>();
            fiber::tests::ServiceInfoTestData service_data;
            service_data.name = "mock-provider";
            service_data.group_name = "DEFAULT_GROUP";
            service_data.checksum = "service-v1";
            service_data.hosts = {
                    make_service_instance("healthy", provider_port),
                    make_service_instance("failing", failing_provider_port),
            };
            const auto service_snapshot = fiber::tests::make_service_info(std::move(service_data));
            (void) primary->service->update(*service_snapshot);
            for (std::size_t i = 0; i < 1024; ++i) {
                std::string candidate = "service-route-" + std::to_string(i);
                const std::uint64_t key = fiber::ai_server::rendezvous_score(candidate, primary->name);
                auto selected = primary->service->select(key, {}, WeightedRendezvous::TimePoint{});
                FIBER_ASSERT(selected.has_value());
                const bool selects_failing = selected->port() == failing_provider_port;
                selected->report(InstanceReportOutcome::Neutral, WeightedRendezvous::TimePoint{});
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
        auto allowed_group = std::make_shared<fiber::ai_server::UserGroupSnapshot>();
        allowed_group->name = "integration-users";
        allowed_group->users.push_back("alice");
        route.allow_user_groups.push_back(std::move(allowed_group));
        if (fallback_enabled_) {
            route.fallback_provider = fallback;
        }
        route.load_balance.max_primary_attempts = 1;
        route.load_balance.fallback_enabled = fallback_enabled_;
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
                    .trace_id = std::string(exchange.request_headers().get("hi-trace-id")),
                    .parent_span_id = std::string(exchange.request_headers().get("hi-span-id-parent")),
                    .span_id = std::string(exchange.request_headers().get("hi-span-id")),
                    .trace_state = std::string(exchange.request_headers().get("tracestate")),
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
        if (reply.pause_before_header) {
            co_await pause_provider();
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
                (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(reply.body.data()),
                                                   reply.body.size(), true);
            }
            co_return;
        }

        for (std::size_t i = 0; i < reply.chunks.size(); ++i) {
            const bool end = i + 1 == reply.chunks.size() && !reply.abort_after_chunks;
            const std::string &chunk = reply.chunks[i];
            auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(chunk.data()),
                                                       chunk.size(), end);
            if (!written) {
                co_return;
            }
            if (reply.pause_after_chunks == i + 1) {
                co_await pause_provider();
            }
        }
        if (reply.abort_after_chunks) {
            (void) exchange.abort(fiber::common::IoErr::Canceled);
        } else if (reply.chunks.empty()) {
            (void) co_await exchange.write_all(nullptr, 0, true);
        }
    }

    fiber::async::Task<void> handle_failing_provider(fiber::http::HttpExchange &exchange) noexcept {
        failing_provider_calls_.fetch_add(1, std::memory_order_release);
        (void) exchange.abort(fiber::common::IoErr::Canceled);
        co_return;
    }

    fiber::async::Task<void> handle_entry(fiber::http::HttpExchange &exchange) noexcept {
        fiber::ai_server::AiServerCatRequest cat_request(exchange, cat_client_.get());
        if (exchange.uri().path == "/internal/llm/rate-limit/check") {
            {
                std::lock_guard lock(mutex_);
                observed_rate_limits_.push_back(ObservedRateLimitRequest{
                        .path = std::string(exchange.uri().path),
                        .trace_id = std::string(exchange.request_headers().get("hi-trace-id")),
                        .parent_span_id = std::string(exchange.request_headers().get("hi-span-id-parent")),
                        .span_id = std::string(exchange.request_headers().get("hi-span-id")),
                        .trace_state = std::string(exchange.request_headers().get("tracestate")),
                });
            }
            fiber::ai_server::TokenRateLimitHttpHandler handler(rate_limiters_, &cat_request);
            co_await handler.handle_check(exchange);
            co_return;
        }
        if (exchange.uri().path == "/internal/llm/rate-limit/settle") {
            if (fail_settle_) {
                (void) exchange.abort(fiber::common::IoErr::Canceled);
                co_return;
            }
            {
                std::lock_guard lock(mutex_);
                observed_rate_limits_.push_back(ObservedRateLimitRequest{
                        .path = std::string(exchange.uri().path),
                        .trace_id = std::string(exchange.request_headers().get("hi-trace-id")),
                        .parent_span_id = std::string(exchange.request_headers().get("hi-span-id-parent")),
                        .span_id = std::string(exchange.request_headers().get("hi-span-id")),
                        .trace_state = std::string(exchange.request_headers().get("tracestate")),
                });
            }
            fiber::ai_server::TokenRateLimitHttpHandler handler(rate_limiters_, &cat_request);
            co_await handler.handle_settle(exchange);
            co_return;
        }
        entry_calls_.fetch_add(1, std::memory_order_release);
        const LlmWireProtocol protocol = exchange.uri().path == "/v1/chat/completions"
                                                 ? LlmWireProtocol::OpenAiChatCompletions
                                                 : LlmWireProtocol::AnthropicMessages;
        AiServerMetrics::Worker &metrics = metrics_.worker(0);
        metrics.request_started(protocol);
        const auto started = fiber::event::EventLoop::current().now();
        LlmRequestHandler handler(provider_client_, runtime_, coordinator_, metrics, audit_max_record_bytes_);
        co_await handler.handle(exchange, protocol, config_, &cat_request);
        metrics.request_finished(protocol, exchange.response_stats(),
                                 std::chrono::duration_cast<std::chrono::microseconds>(
                                         fiber::event::EventLoop::current().now() - started));
        completed_entry_calls_.fetch_add(1, std::memory_order_release);
    }

    fiber::event::EventLoopGroup *group_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<MockReply> replies_;
    std::vector<ObservedProviderRequest> observed_;
    std::vector<ObservedRateLimitRequest> observed_rate_limits_;
    std::vector<std::vector<std::uint8_t>> cat_frames_;
    std::size_t reply_index_ = 0;
    std::atomic<int> entry_calls_{0};
    std::atomic<int> completed_entry_calls_{0};
    std::atomic<int> failing_provider_calls_{0};
    std::atomic<bool> provider_pause_reached_{false};
    std::atomic<bool> provider_pause_released_{false};
    bool fail_settle_ = false;
    bool service_rendezvous_ = false;
    std::size_t audit_max_record_bytes_ = 0;
    bool enable_cat_ = false;
    bool primary_dns_timeout_ = false;
    bool fallback_enabled_ = true;
    std::string rendezvous_route_key_;
    std::unique_ptr<fiber::cat::CatClient> cat_client_;
    fiber::ai_server::TokenRateLimitService rate_limiters_;
    fiber::ai_server::RateLimitShardRing ring_;
    fiber::ai_server::TokenRateLimitRemoteClient remote_client_;
    fiber::ai_server::TokenRateLimitCoordinator coordinator_;
    fiber::ai_server::ProviderConnectionManager connections_;
    fiber::ai_server::ProviderHttpClient provider_client_;
    AiServerMetrics metrics_;
    fiber::ai_server::ProviderRuntimeRegistry runtime_;
    std::shared_ptr<const LlmConfigSnapshot> config_;
    fiber::net::TcpListener cat_collector_;
    fiber::async::WaitGroup cat_collector_tasks_;
    fiber::http::Http1Server provider_server_;
    fiber::http::Http1Server failing_provider_server_;
    fiber::http::Http1Server entry_server_;
    bool initialized_ = false;
};

class FixtureHarness {
public:
    explicit FixtureHarness(std::vector<MockReply> replies, bool fail_settle = false, bool service_rendezvous = false,
                            std::size_t audit_max_record_bytes = fiber::ai_server::kDefaultLlmAuditMaxRecordBytes,
                            bool enable_cat = false, fiber::ai_server::WorkerDnsService::Options dns_options = {},
                            bool primary_dns_timeout = false, bool fallback_enabled = true) {
        fiber::log::LoggerManager::global().shutdown();
        char path[] = "/tmp/fiber-llm-audit-XXXXXX";
        const int fd = ::mkstemp(path);
        if (fd < 0) {
            return;
        }
        (void) ::close(fd);
        audit_path_ = path;
        const std::string logging_config =
                R"({
  "version": 1,
  "queue": {"capacity_bytes": 67108864},
  "appenders": [
    {
      "name": "test_stderr",
      "type": "console",
      "stream": "stderr",
      "min_level": "fatal",
      "max_level": "fatal"
    }
  ],
  "root_logger": {
    "level": "fatal",
    "appenders": ["test_stderr"]
  },
  "loggers": [],
  "audit": {
    "path": ")" +
                audit_path_ + R"(",
    "max_record_bytes": )" +
                std::to_string(audit_max_record_bytes) + R"(,
    "rotate_bytes": 0,
    "max_archives": 30
  }
})";
        auto log_config =
                fiber::ai_server::parse_ai_server_log_config(logging_config, "/tmp/ai-server-test-logging.json");
        if (!log_config) {
            (void) ::unlink(audit_path_.c_str());
            audit_path_.clear();
            return;
        }
        audit_appender_id_ = log_config->audit_appender_id;
        if (!fiber::log::LoggerManager::global().initialize(std::move(log_config->config))) {
            (void) ::unlink(audit_path_.c_str());
            audit_path_.clear();
            return;
        }
        logging_started_ = true;
        fixture_ = std::make_unique<ProxyFixture>(group_, std::move(replies), fail_settle, service_rendezvous,
                                                  audit_max_record_bytes, enable_cat, std::move(dns_options),
                                                  primary_dns_timeout, fallback_enabled);
        group_.start();
        groups_started_ = true;
        std::promise<FixturePorts> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, &promise]() { return fixture_->start(&promise); });
        if (future.wait_for(5s) == std::future_status::ready) {
            ports_ = future.get();
        }
    }

    ~FixtureHarness() {
        if (groups_started_) {
            fixture_->release_provider();
            std::promise<void> promise;
            auto future = promise.get_future();
            fiber::async::spawn(group_.at(0), [this, &promise]() { return fixture_->shutdown(&promise); });
            (void) future.wait_for(5s);
            group_.stop();
            group_.join();
        }
        fixture_.reset();
        if (logging_started_) {
            fiber::log::LoggerManager::global().shutdown();
        }
        if (!audit_path_.empty()) {
            (void) ::unlink(audit_path_.c_str());
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return fixture_ && logging_started_ && ports_.entry != 0 && ports_.provider != 0;
    }

    [[nodiscard]] std::uint16_t entry_port() const noexcept { return ports_.entry; }

    [[nodiscard]] std::vector<ObservedProviderRequest> observed() const { return fixture_->observed(); }
    [[nodiscard]] std::vector<ObservedRateLimitRequest> observed_rate_limits() const {
        return fixture_->observed_rate_limits();
    }

    [[nodiscard]] bool wait_for_rate_limit_requests(std::size_t count) const {
        for (std::size_t i = 0; i < 5000; ++i) {
            if (fixture_->observed_rate_limits().size() >= count) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    [[nodiscard]] bool wait_for_cat_frame(std::string_view first, std::string_view second = {}) const {
        for (std::size_t i = 0; i < 5000; ++i) {
            if (fixture_->cat_frame_contains(first, second)) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    [[nodiscard]] bool cat_frame_contains(std::string_view first, std::string_view second = {}) const {
        return fixture_->cat_frame_contains(first, second);
    }

    [[nodiscard]] int entry_calls() const noexcept { return fixture_->entry_calls(); }
    [[nodiscard]] int completed_entry_calls() const noexcept { return fixture_->completed_entry_calls(); }
    [[nodiscard]] int failing_provider_calls() const noexcept { return fixture_->failing_provider_calls(); }
    [[nodiscard]] std::string rendezvous_route_key() const { return std::string(fixture_->rendezvous_route_key()); }

    [[nodiscard]] bool wait_for_provider_pause() const noexcept {
        for (std::size_t i = 0; i < 5000; ++i) {
            if (fixture_->provider_paused()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void release_provider() noexcept { fixture_->release_provider(); }

    [[nodiscard]] bool wait_for_completed_entry_calls(int count) const noexcept {
        for (std::size_t i = 0; i < 5000; ++i) {
            if (fixture_->completed_entry_calls() >= count) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    [[nodiscard]] bool wait_for_audit_records(std::uint64_t count) const noexcept {
        for (std::size_t i = 0; i < 5000; ++i) {
            fiber::log::LoggerManager::global().flush();
            if (fiber::log::LoggerManager::global().appender_stats(audit_appender_id_).written_records >= count) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    [[nodiscard]] std::string audit_contents() const {
        fiber::log::LoggerManager::global().flush();
        std::ifstream input(audit_path_, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] fiber::log::AppenderStats audit_stats() const noexcept {
        fiber::log::LoggerManager::global().flush();
        return fiber::log::LoggerManager::global().appender_stats(audit_appender_id_);
    }

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
    std::string audit_path_;
    std::unique_ptr<ProxyFixture> fixture_;
    FixturePorts ports_;
    fiber::log::AppenderId audit_appender_id_ = fiber::log::kInvalidAppenderId;
    bool groups_started_ = false;
    bool logging_started_ = false;
};

TEST(LlmProxyIntegrationTest, RetriesTransportAuthRateLimitAndFallbackFromOriginalJson) {
    FixtureHarness fixture(
            {
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
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
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
    EXPECT_TRUE(fixture.wait_for_cat_frame(cat_nt1_type_and_name("LLM.UpstreamError", "read_header")));
    EXPECT_TRUE(fixture.wait_for_cat_frame(cat_nt1_type_and_name("LLM.UpstreamError", "upstream_error")));
}

TEST(LlmProxyIntegrationTest, DnsTimeoutSkipsRemainingTokensAndBacksOffRepeatedLookup) {
    std::uint16_t dns_port = 0;
    const int dns_fd = bind_dropping_dns_socket(dns_port);
    ASSERT_GE(dns_fd, 0);
    ASSERT_NE(dns_port, 0);

    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"first","usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}})",
                    },
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"second","usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}})",
                    },
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true,
            fiber::ai_server::WorkerDnsService::Options{
                    .nameserver = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), dns_port),
                    .timeout = 20ms,
                    .transient_failure_ttl = 1s,
                    .attempts = 1,
            },
            true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse first =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":false,"messages":[]})");
    ASSERT_EQ(first.system_error, 0);
    ASSERT_EQ(first.status, 200);
    EXPECT_NE(first.body.find(R"("id":"first")"), std::string::npos);
    EXPECT_EQ(drain_dns_packets(dns_fd), 2u);

    const RawHttpResponse second =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":false,"messages":[]})");
    ASSERT_EQ(second.system_error, 0);
    ASSERT_EQ(second.status, 200);
    EXPECT_NE(second.body.find(R"("id":"second")"), std::string::npos);
    EXPECT_EQ(drain_dns_packets(dns_fd), 0u);

    const auto observed = fixture.observed();
    ASSERT_EQ(observed.size(), 2u);
    EXPECT_EQ(bearer_token_name(observed[0].authorization), "token-f");
    EXPECT_EQ(bearer_token_name(observed[1].authorization), "token-f");
    EXPECT_NE(observed[0].body.find("upstream-fallback"), std::string::npos);
    EXPECT_NE(observed[1].body.find("upstream-fallback"), std::string::npos);

    ASSERT_TRUE(fixture.wait_for_audit_records(2));
    const std::string audit = fixture.audit_contents();
    EXPECT_NE(audit.find(R"("schema_version":5)"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"failure_phase\":\"dns\")"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"io_error\":\"timed_out\")"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"failure_source\":\"io\")"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"failure_source\":\"dns_backoff\")"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"retry_target\":\"next_provider\")"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"retry_performed\":true,\"skipped_attempts\":2)"), std::string::npos);
    EXPECT_NE(audit.find(R"("provider_attempt_count":2,"provider_attempt_skipped_count":2)"), std::string::npos);
    EXPECT_TRUE(fixture.wait_for_cat_frame("io_error=timed_out", "failure_source=io"));
    EXPECT_TRUE(fixture.wait_for_cat_frame("failure_source=dns_backoff", "retry_target=next_provider"));
    EXPECT_TRUE(fixture.wait_for_cat_frame("retry_target=next_provider", "skipped_attempts=2"));
    EXPECT_TRUE(fixture.wait_for_cat_frame("retry_performed=true", "skipped_attempts=2"));
    EXPECT_TRUE(fixture.wait_for_cat_frame(cat_nt1_type_and_name("LLM.UpstreamError", "dns")));
    EXPECT_FALSE(fixture.cat_frame_contains("failure_phase="));

    auto metrics = fixture.settlement_metrics();
    ASSERT_TRUE(metrics);
    EXPECT_NE(metrics->find("ai_server_provider_transport_failures_total{protocol=\"openai\",phase=\"dns\"} 2"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_provider_attempts_skipped_total{protocol=\"openai\"} 4"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_dns_backoff_hits_total{protocol=\"openai\"} 1"), std::string::npos);
    (void) ::close(dns_fd);
}

TEST(LlmProxyIntegrationTest, DnsTimeoutWithoutFallbackReturnsCurrentTransportError) {
    std::uint16_t dns_port = 0;
    const int dns_fd = bind_dropping_dns_socket(dns_port);
    ASSERT_GE(dns_fd, 0);
    ASSERT_NE(dns_port, 0);

    FixtureHarness fixture(
            {}, false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, false,
            fiber::ai_server::WorkerDnsService::Options{
                    .nameserver = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), dns_port),
                    .timeout = 20ms,
                    .transient_failure_ttl = 1s,
                    .attempts = 1,
            },
            true, false);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse response =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":false,"messages":[]})");
    ASSERT_EQ(response.system_error, 0);
    EXPECT_EQ(response.status, 502);
    EXPECT_NE(response.body.find("provider_transport_error"), std::string::npos);
    EXPECT_TRUE(fixture.observed().empty());
    EXPECT_EQ(drain_dns_packets(dns_fd), 2u);

    ASSERT_TRUE(fixture.wait_for_audit_records(1));
    const std::string audit = fixture.audit_contents();
    EXPECT_NE(audit.find(R"(\"failure_phase\":\"dns\")"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"retry_target\":\"next_provider\",\"retryable\":true,\"retry_performed\":false)"),
              std::string::npos);
    EXPECT_NE(audit.find(R"("provider_attempt_count":1,"provider_attempt_skipped_count":2)"), std::string::npos);
    EXPECT_NE(audit.find(R"("error_json":"transport_error")"), std::string::npos);
    EXPECT_NE(audit.find(R"("usage_json":{"promptTokens":0,"completionTokens":0,"total_tokens":0})"),
              std::string::npos);
    (void) ::close(dns_fd);
}

TEST(LlmProxyIntegrationTest, PropagatesCatContextToRemoteLimiterAndEveryProviderAttempt) {
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 401,
                            .body = R"({"error":{"type":"authentication_error","code":"invalid_api_key"}})",
                    },
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"ok","usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})",
                    },
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse auth_error =
            post_json(fixture.entry_port(), "invalid-token", R"({"model":"logical","stream":false})",
                      ClientCatHeaders{
                              .trace_id = "auth-error-root",
                      });
    EXPECT_EQ(auth_error.system_error, 0);
    EXPECT_EQ(auth_error.status, 401);
    EXPECT_TRUE(auth_error.complete);
    EXPECT_EQ(auth_error.trace_id, "auth-error-root");

    const RawHttpResponse response = post_json(fixture.entry_port(), token, R"({"model":"logical","stream":false})",
                                               ClientCatHeaders{
                                                       .trace_id = "upstream-root",
                                                       .parent_span_id = "upstream-parent",
                                                       .span_id = "upstream-span",
                                                       .trace_state = "tenant=blue",
                                               });

    EXPECT_EQ(response.system_error, 0);
    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.complete);
    EXPECT_EQ(response.trace_id, "upstream-root");
    const auto providers = fixture.observed();
    ASSERT_EQ(providers.size(), 2U);
    for (const ObservedProviderRequest &provider: providers) {
        EXPECT_EQ(provider.trace_id, "upstream-root");
        EXPECT_EQ(provider.parent_span_id, "upstream-span");
        EXPECT_FALSE(provider.span_id.empty());
        EXPECT_EQ(provider.trace_state, "tenant=blue");
    }
    EXPECT_NE(providers[0].span_id, providers[1].span_id);
    EXPECT_TRUE(fixture.wait_for_cat_frame("LLM.Provider", "RemoteCall"));

    ASSERT_TRUE(fixture.wait_for_rate_limit_requests(2));
    const auto rate_limits = fixture.observed_rate_limits();
    ASSERT_GE(rate_limits.size(), 2U);
    EXPECT_EQ(rate_limits[0].path, "/internal/llm/rate-limit/check");
    EXPECT_EQ(rate_limits[1].path, "/internal/llm/rate-limit/settle");
    for (const ObservedRateLimitRequest &rate_limit: rate_limits) {
        EXPECT_EQ(rate_limit.trace_id, "upstream-root");
        EXPECT_EQ(rate_limit.parent_span_id, "upstream-span");
        EXPECT_FALSE(rate_limit.span_id.empty());
        EXPECT_EQ(rate_limit.trace_state, "tenant=blue");
        EXPECT_NE(rate_limit.span_id, providers[0].span_id);
        EXPECT_NE(rate_limit.span_id, providers[1].span_id);
    }
    EXPECT_NE(rate_limits[0].span_id, rate_limits[1].span_id);

    const RawHttpResponse internal_error =
            post_json(fixture.entry_port(), {}, "{}", ClientCatHeaders{.trace_id = "internal-error-root"},
                      "/internal/llm/rate-limit/check");
    EXPECT_EQ(internal_error.system_error, 0);
    EXPECT_EQ(internal_error.status, 400);
    EXPECT_TRUE(internal_error.complete);
    EXPECT_EQ(internal_error.trace_id, "internal-error-root");
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

TEST(LlmProxyIntegrationTest, CatUrlTransactionNamesIncludeAuthorizedModelForBothProtocols) {
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"openai","choices":[],"usage":{"prompt_tokens":2,"completion_tokens":3}})",
                    },
                    MockReply{
                            .status = 200,
                            .content_type = "text/event-stream",
                            .chunks =
                                    {
                                            "event: message_start\n"
                                            "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_"
                                            "tokens\":2,\"output_tokens\":0}}}\n\n",
                                            "event: content_block_start\n"
                                            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                            "\"type\":\"text\",\"text\":\"\"}}\n\n"
                                            "event: content_block_delta\n"
                                            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                            "\"type\":\"text_delta\",\"text\":\"hello\"}}\n\n",
                                            "event: message_delta\n"
                                            "data: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":3}}\n\n"
                                            "event: message_stop\n"
                                            "data: {\"type\":\"message_stop\"}\n\n",
                                    },
                            .stream = true,
                    },
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"anthropic-alias","type":"message","role":"assistant","content":[],"model":"upstream-anthropic-primary","stop_reason":"end_turn","usage":{"input_tokens":2,"output_tokens":3}})",
                    },
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"zhangwang","choices":[],"usage":{"prompt_tokens":2,"completion_tokens":3}})",
                    },
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    const std::string zhangwang_token = issue_token("zhangwang");
    const std::string mallory_token = issue_token("mallory");
    ASSERT_FALSE(token.empty());
    ASSERT_FALSE(zhangwang_token.empty());
    ASSERT_FALSE(mallory_token.empty());

    const RawHttpResponse openai =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":false,"messages":[]})");
    const RawHttpResponse anthropic =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":true,"max_tokens":16,"messages":[]})",
                      {}, "/v1/messages");
    const RawHttpResponse anthropic_alias = post_json(
            fixture.entry_port(), token, R"({"model":"logical","max_tokens":16,"messages":[]})", {}, "/v1/message");
    const RawHttpResponse zhangwang =
            post_json(fixture.entry_port(), zhangwang_token, R"({"model":"logical","stream":false,"messages":[]})");
    const RawHttpResponse mallory =
            post_json(fixture.entry_port(), mallory_token, R"({"model":"logical","stream":false,"messages":[]})");

    EXPECT_EQ(openai.system_error, 0);
    EXPECT_EQ(openai.status, 200);
    EXPECT_TRUE(openai.complete);
    EXPECT_EQ(anthropic.system_error, 0);
    EXPECT_EQ(anthropic.status, 200);
    EXPECT_TRUE(anthropic.complete);
    EXPECT_EQ(anthropic_alias.system_error, 0);
    EXPECT_EQ(anthropic_alias.status, 200);
    EXPECT_TRUE(anthropic_alias.complete);
    EXPECT_EQ(zhangwang.system_error, 0);
    EXPECT_EQ(zhangwang.status, 200);
    EXPECT_TRUE(zhangwang.complete);
    EXPECT_EQ(mallory.system_error, 0);
    EXPECT_EQ(mallory.status, 403);
    EXPECT_TRUE(mallory.complete);
    EXPECT_TRUE(fixture.wait_for_cat_frame("/v1/chat/completions:logical", "stream=false"));
    EXPECT_TRUE(fixture.wait_for_cat_frame("/v1/messages:logical", "stream=true"));
    EXPECT_TRUE(fixture.wait_for_cat_frame("/v1/message:logical", "stream=false"));
    EXPECT_TRUE(fixture.wait_for_cat_frame(cat_nt1_type_name_and_status("Auth", "alice", fiber::cat::status::Success),
                                           "allowed_user_group=integration-users"));
    EXPECT_TRUE(
            fixture.wait_for_cat_frame(cat_nt1_type_name_and_status("Auth", "zhangwang", fiber::cat::status::Success)));
    EXPECT_TRUE(fixture.wait_for_cat_frame(cat_nt1_type_name_and_status("Auth", "mallory", fiber::cat::status::Error)));
    EXPECT_FALSE(fixture.cat_frame_contains(cat_nt1_type_and_name("Auth", "zhangwang"), "allowed_user_group="));
    EXPECT_FALSE(fixture.cat_frame_contains(cat_nt1_type_and_name("Auth", "mallory"), "allowed_user_group="));
    EXPECT_TRUE(fixture.wait_for_cat_frame("LLM.Provider", "time_to_response_header_us="));
    EXPECT_FALSE(fixture.cat_frame_contains("failure_phase="));
    EXPECT_FALSE(fixture.cat_frame_contains("io_error="));
    EXPECT_FALSE(fixture.cat_frame_contains("failure_source="));
    EXPECT_FALSE(fixture.cat_frame_contains("retry_target="));
    EXPECT_FALSE(fixture.cat_frame_contains("retryable="));
    EXPECT_FALSE(fixture.cat_frame_contains("retry_performed="));
    EXPECT_FALSE(fixture.cat_frame_contains("skipped_attempts="));
    EXPECT_FALSE(fixture.cat_frame_contains("response_started="));
    EXPECT_FALSE(fixture.cat_frame_contains("outcome="));
    EXPECT_TRUE(fixture.wait_for_cat_frame("LLM.Provider", "reuse_count=0"));
    EXPECT_TRUE(fixture.wait_for_cat_frame("LLM.Provider", "reuse_count=1"));
    EXPECT_FALSE(fixture.cat_frame_contains("connection_request_count="));
    EXPECT_FALSE(fixture.cat_frame_contains("connection_reuse_count="));
    EXPECT_TRUE(fixture.wait_for_cat_frame("upstream_model=upstream-anthropic-primary", "time_to_first_token_us="));
    EXPECT_FALSE(fixture.cat_frame_contains("upstream_model=upstream-primary", "time_to_first_token_us="));
    EXPECT_TRUE(fixture.wait_for_cat_frame("LLM.Provider", "body_transfer_us="));
    EXPECT_FALSE(fixture.cat_frame_contains("LLM.UpstreamError"));
}

TEST(LlmProxyIntegrationTest, CatUrlTransactionUsesSpaceSeparatedDataAndBoundsUserAgent) {
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"normal","choices":[]})",
                    },
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"truncated","choices":[]})",
                    },
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request = R"({"model":"logical","stream":false,"messages":[]})";
    constexpr std::string_view user_agent = "ai-server-integration/1.0 with spaces";

    const RawHttpResponse normal =
            post_json(fixture.entry_port(), token, request, ClientCatHeaders{.user_agent = std::string(user_agent)});
    std::string oversized_user_agent(fiber::ai_server::kMaxAiServerCatUserAgentBytes + 16, 'u');
    const RawHttpResponse truncated = post_json(fixture.entry_port(), token, request,
                                                ClientCatHeaders{.user_agent = std::move(oversized_user_agent)});

    EXPECT_EQ(normal.status, 200);
    EXPECT_TRUE(normal.complete);
    EXPECT_EQ(truncated.status, 200);
    EXPECT_TRUE(truncated.complete);

    const std::string root_name = cat_nt1_type_and_name("URL", "/v1/chat/completions:logical");
    const std::string root_data =
            "method=POST host=127.0.0.1 content_type=application/json trace_context=new protocol=openai "
            "user=alice kid=test1 stream=false model=logical status=200 user_agent=" +
            std::string(user_agent);
    EXPECT_TRUE(fixture.wait_for_cat_frame(root_name, root_data));
    EXPECT_FALSE(fixture.cat_frame_contains("&host="));
    EXPECT_FALSE(fixture.cat_frame_contains("&protocol="));

    const std::string bounded_user_agent(fiber::ai_server::kMaxAiServerCatUserAgentBytes, 'u');
    EXPECT_TRUE(fixture.wait_for_cat_frame(root_name, "user_agent_truncated=true user_agent=" + bounded_user_agent));
    EXPECT_FALSE(fixture.cat_frame_contains(std::string(fiber::ai_server::kMaxAiServerCatUserAgentBytes + 1, 'u')));
}

TEST(LlmProxyIntegrationTest, CatProviderTransactionOmitsFirstTokenAndBodyTransferForEmptyResponse) {
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                    },
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse response =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":false,"messages":[]})");

    EXPECT_EQ(response.system_error, 0);
    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.complete);
    EXPECT_TRUE(response.body.empty());
    EXPECT_TRUE(fixture.wait_for_cat_frame("LLM.Provider", "time_to_response_header_us="));
    EXPECT_FALSE(fixture.cat_frame_contains("LLM.Provider", "time_to_first_token_us="));
    EXPECT_FALSE(fixture.cat_frame_contains("LLM.Provider", "body_transfer_us="));
}

TEST(LlmProxyIntegrationTest, CatRecordsGeneratedResponseErrorAlongsideInvalidProviderResponse) {
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .content_type = "application/json",
                            .body = R"({"error":"streaming unavailable"})",
                    },
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse response =
            post_json(fixture.entry_port(), token, R"({"model":"logical","stream":true,"messages":[]})");

    EXPECT_EQ(response.system_error, 0);
    EXPECT_EQ(response.status, 502);
    EXPECT_TRUE(response.complete);
    EXPECT_TRUE(fixture.wait_for_cat_frame(cat_nt1_type_and_name("LLM.UpstreamError", "invalid_response"),
                                           "io_error=invalid&failure_source=io&status=200"));
    EXPECT_TRUE(fixture.wait_for_cat_frame(
            cat_nt1_type_name_and_status("LLM.ResponseError", "upstream_invalid_response", fiber::cat::status::Error),
            "status=502&type=api_error&message=provider did not return an event stream"));
}

TEST(LlmProxyIntegrationTest, RelaysSseBytesUnchangedAndNeverRetriesAfterResponseStart) {
    constexpr std::string_view expected =
            ": ping\r\nid: 7\r\ndata:{\"choices\":[],\"usage\":{\"prompt_tokens\":2"
            ",\"completion_tokens\":3,\"total_tokens\":5}}\r\n\r\ndata: [DONE]\r\n\r\ndata: [DONE]\n\n";
    FixtureHarness fixture(
            {
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
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({"model":"logical","stream":true,"messages":[{"role":"user","content":"hello"}]})";

    const RawHttpResponse success = post_json(fixture.entry_port(), token, request,
                                              ClientCatHeaders{
                                                      .trace_id = "sse-success-root",
                                                      .span_id = "sse-success-span",
                                              });
    EXPECT_EQ(success.status, 200);
    EXPECT_TRUE(success.complete);
    EXPECT_EQ(success.trace_id, "sse-success-root");
    EXPECT_EQ(success.body, expected);

    const RawHttpResponse truncated = post_json(fixture.entry_port(), token, request,
                                                ClientCatHeaders{
                                                        .trace_id = "sse-truncated-root",
                                                        .span_id = "sse-truncated-span",
                                                });
    EXPECT_EQ(truncated.status, 200);
    EXPECT_TRUE(truncated.complete);
    EXPECT_EQ(truncated.trace_id, "sse-truncated-root");
    EXPECT_EQ(truncated.body.find("data: [DONE]"), std::string::npos);
    EXPECT_EQ(fixture.observed().size(), 2u);
    EXPECT_TRUE(fixture.wait_for_cat_frame("upstream_model=upstream-primary", "body_transfer_us="));
    EXPECT_FALSE(fixture.cat_frame_contains("upstream_model=upstream-primary", "time_to_first_token_us="));
}

TEST(LlmProxyIntegrationTest, DrainsSseUsageAfterClientDisconnectAndDoesNotRetry) {
    constexpr std::string_view first_event = "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n";
    // Fill one relay read budget so the first event reaches the client before the mock Provider pauses.
    std::string first_chunk = ":";
    first_chunk.append(64 * 1024 - first_event.size() - 2, 'x');
    first_chunk.push_back('\n');
    first_chunk.append(first_event);
    FixtureHarness fixture({
            MockReply{
                    .status = 200,
                    .content_type = "text/event-stream",
                    .chunks =
                            {
                                    std::move(first_chunk),
                                    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,"
                                    "\"completion_tokens\":3,\"total_tokens\":5}}\n\ndata: [DONE]\n\n",
                            },
                    .stream = true,
                    .pause_after_chunks = 1,
            },
            MockReply{
                    .status = 200,
                    .content_type = "text/event-stream",
                    .chunks = {"data: {\"choices\":[]}\n\n"},
                    .stream = true,
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({"model":"logical","stream":true,"messages":[{"role":"user","content":"hello"}]})";

    const int client = open_json_request(fixture.entry_port(), token, request);
    ASSERT_GE(client, 0);
    if (!fixture.wait_for_provider_pause()) {
        reset_http_client(client);
        FAIL() << "Provider did not pause after the first SSE chunk";
    }
    std::string received;
    if (!wait_for_socket_text(client, first_event, received)) {
        reset_http_client(client);
        FAIL() << "did not receive the first SSE event: " << received;
    }
    reset_http_client(client);
    std::this_thread::sleep_for(20ms);
    fixture.release_provider();

    ASSERT_TRUE(fixture.wait_for_completed_entry_calls(1));
    ASSERT_TRUE(fixture.wait_for_rate_limit_requests(2));
    ASSERT_TRUE(fixture.wait_for_audit_records(1));
    EXPECT_EQ(fixture.observed().size(), 1u);

    auto metrics = fixture.settlement_metrics();
    ASSERT_TRUE(metrics);
    EXPECT_NE(metrics->find("ai_server_provider_attempts_total{protocol=\"openai\"} 1"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_provider_retries_total{protocol=\"openai\"} 0"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_sse_drains_total{protocol=\"openai\",result=\"completed\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_rate_limit_settlements_total{result=\"usage\"} 1"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"in_nocache\"} 2"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"out\"} 3"),
              std::string::npos);

    const std::string audit = fixture.audit_contents();
    EXPECT_NE(audit.find(R"("provider_attempt_count":1)"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"response_started\":true,\"outcome\":\"success\")"), std::string::npos);
    EXPECT_NE(audit.find(R"("output_complete":true,"output_canonical_complete":true)"), std::string::npos);
    EXPECT_NE(audit.find(R"("client_aborted":true,"error_json":"conn_reset")"), std::string::npos);
    EXPECT_NE(audit.find(R"("usage_json":{"promptTokens":2,"completionTokens":3,"total_tokens":5})"),
              std::string::npos);
}

TEST(LlmProxyIntegrationTest, DoesNotRetryWhenUpstreamFailsWhileDrainingAfterClientDisconnect) {
    constexpr std::string_view first_event = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
    // Fill one relay read budget so the first event reaches the client before the mock Provider pauses.
    std::string first_chunk = ":";
    first_chunk.append(64 * 1024 - first_event.size() - 2, 'x');
    first_chunk.push_back('\n');
    first_chunk.append(first_event);
    std::string drain_chunk = ":";
    drain_chunk.append(64 * 1024 - 2, 'y');
    drain_chunk.push_back('\n');
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .content_type = "text/event-stream",
                            .chunks = {std::move(first_chunk), std::move(drain_chunk)},
                            .stream = true,
                            .abort_after_chunks = true,
                            .pause_after_chunks = 1,
                    },
                    MockReply{
                            .status = 200,
                            .content_type = "text/event-stream",
                            .chunks = {"data: {\"choices\":[]}\n\n"},
                            .stream = true,
                    },
            },
            false, false, fiber::ai_server::kDefaultLlmAuditMaxRecordBytes, true);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({"model":"logical","stream":true,"messages":[{"role":"user","content":"hello"}]})";

    const int client = open_json_request(fixture.entry_port(), token, request);
    ASSERT_GE(client, 0);
    if (!fixture.wait_for_provider_pause()) {
        reset_http_client(client);
        FAIL() << "Provider did not pause after the first SSE chunk";
    }
    std::string received;
    if (!wait_for_socket_text(client, first_event, received)) {
        reset_http_client(client);
        FAIL() << "did not receive the first SSE event: " << received;
    }
    reset_http_client(client);
    std::this_thread::sleep_for(20ms);
    fixture.release_provider();

    ASSERT_TRUE(fixture.wait_for_completed_entry_calls(1));
    ASSERT_TRUE(fixture.wait_for_rate_limit_requests(2));
    ASSERT_TRUE(fixture.wait_for_audit_records(1));
    EXPECT_EQ(fixture.observed().size(), 1u);

    auto metrics = fixture.settlement_metrics();
    ASSERT_TRUE(metrics);
    EXPECT_NE(metrics->find("ai_server_provider_attempts_total{protocol=\"openai\"} 1"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_provider_retries_total{protocol=\"openai\"} 0"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_provider_failures_total{protocol=\"openai\"} 1"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_sse_drains_total{protocol=\"openai\",result=\"upstream_error\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_rate_limit_settlements_total{result=\"no_usage\"} 1"), std::string::npos);

    const std::string audit = fixture.audit_contents();
    EXPECT_NE(audit.find(R"("provider_attempt_count":1)"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"response_started\":true,\"outcome\":\"stream_error\")"), std::string::npos);
    EXPECT_NE(audit.find(R"("output_complete":false,"output_canonical_complete":false)"), std::string::npos);
    EXPECT_NE(audit.find(R"("client_aborted":true,"error_json":"conn_reset")"), std::string::npos) << audit;
    EXPECT_TRUE(fixture.wait_for_cat_frame("upstream_model=upstream-primary", "time_to_first_token_us="));
    EXPECT_FALSE(fixture.cat_frame_contains("upstream_model=upstream-primary", "body_transfer_us="));
    EXPECT_TRUE(fixture.wait_for_cat_frame(cat_nt1_type_and_name("LLM.UpstreamError", "read_body"),
                                           "response_started=true"));
}

TEST(LlmProxyIntegrationTest, DrainsSseWhenClientDisconnectsBeforeResponseHeader) {
    FixtureHarness fixture({
            MockReply{
                    .status = 200,
                    .content_type = "text/event-stream",
                    .chunks =
                            {
                                    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4,"
                                    "\"completion_tokens\":6,\"total_tokens\":10}}\n\ndata: [DONE]\n\n",
                            },
                    .stream = true,
                    .pause_before_header = true,
            },
            MockReply{
                    .status = 200,
                    .content_type = "text/event-stream",
                    .chunks = {"data: {\"choices\":[]}\n\n"},
                    .stream = true,
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({"model":"logical","stream":true,"messages":[{"role":"user","content":"hello"}]})";

    const int client = open_json_request(fixture.entry_port(), token, request);
    ASSERT_GE(client, 0);
    if (!fixture.wait_for_provider_pause()) {
        reset_http_client(client);
        FAIL() << "Provider did not pause before the response header";
    }
    reset_http_client(client);
    std::this_thread::sleep_for(20ms);
    fixture.release_provider();

    ASSERT_TRUE(fixture.wait_for_completed_entry_calls(1));
    ASSERT_TRUE(fixture.wait_for_rate_limit_requests(2));
    ASSERT_TRUE(fixture.wait_for_audit_records(1));
    EXPECT_EQ(fixture.observed().size(), 1u);

    auto metrics = fixture.settlement_metrics();
    ASSERT_TRUE(metrics);
    EXPECT_NE(metrics->find("ai_server_provider_attempts_total{protocol=\"openai\"} 1"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_provider_retries_total{protocol=\"openai\"} 0"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_sse_drains_total{protocol=\"openai\",result=\"completed\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_rate_limit_settlements_total{result=\"usage\"} 1"), std::string::npos);
    EXPECT_NE(metrics->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"in_nocache\"} 4"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"out\"} 6"),
              std::string::npos);

    const std::string audit = fixture.audit_contents();
    EXPECT_NE(audit.find(R"("provider_attempt_count":1)"), std::string::npos);
    EXPECT_NE(audit.find(R"(\"response_started\":false,\"outcome\":\"success\")"), std::string::npos);
    EXPECT_NE(audit.find(R"("client_aborted":true,"error_json":"conn_reset")"), std::string::npos) << audit;
    EXPECT_NE(audit.find(R"("usage_json":{"promptTokens":4,"completionTokens":6,"total_tokens":10})"),
              std::string::npos);
    EXPECT_NE(audit.find(R"("response_header_sent":false)"), std::string::npos);
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
    EXPECT_NE(metrics->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"in_cache\"} 0"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"in_nocache\"} 6"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"out\"} 9"),
              std::string::npos);
    EXPECT_NE(metrics->find("ai_server_provider_token_usage_total{provider_name=\"primary\",protocol=\"openai\","
                            "token_type=\"in_nocache\"} 6"),
              std::string::npos);
}

TEST(LlmProxyIntegrationTest, EmitsOneJsonAuditLineWithInputAndOutput) {
    FixtureHarness fixture({
            MockReply{
                    .status = 200,
                    .body = R"({"id":"ok","choices":[{"message":{"role":"assistant","content":"weather is sunny","tool_calls":[{"type":"function","function":{"name":"weather","arguments":"{\"city\":\"Paris\"}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":8,"completion_tokens":6,"total_tokens":14,"prompt_tokens_details":{"cached_tokens":3}}})",
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());
    constexpr std::string_view request =
            R"({"model":"logical","stream":false,"messages":[{"role":"system","content":"answer briefly"},{"role":"user","content":[{"type":"text","text":"weather in Paris"},{"type":"image_url","image_url":{"url":"https://example.test/image?signature=SECRET_URL"}},{"type":"input_audio","input_audio":{"data":"SECRET_BASE64"}}]}],"tools":[{"type":"function","function":{"name":"weather","description":"look up weather","parameters":{"type":"object"}}}]})";

    const RawHttpResponse response =
            post_json(fixture.entry_port(), token, request,
                      ClientCatHeaders{.user_agent = "audit-client/1.0", .real_ip = "203.0.113.9"});

    ASSERT_EQ(response.system_error, 0);
    ASSERT_EQ(response.status, 200);
    ASSERT_TRUE(response.complete);
    ASSERT_TRUE(fixture.wait_for_audit_records(1));
    const std::string logs = fixture.audit_contents();
    constexpr std::string_view marker = R"("event":"llm_request")";
    const std::size_t marker_pos = logs.find(marker);
    ASSERT_NE(marker_pos, std::string::npos) << logs;
    EXPECT_EQ(logs.find(marker, marker_pos + marker.size()), std::string::npos);
    ASSERT_FALSE(logs.empty());
    ASSERT_EQ(logs.back(), '\n');
    EXPECT_EQ(logs.find('\n'), logs.size() - 1);
    const std::string_view audit_json(logs.data(), logs.size() - 1);
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
    const auto *attempts_json_field = root.as_object().find("attempts_json");
    ASSERT_NE(attempts_json_field, nullptr);
    ASSERT_TRUE(attempts_json_field->value.is_text());
    const auto *rate_limit_json_field = root.as_object().find("rate_limit_json");
    ASSERT_NE(rate_limit_json_field, nullptr);
    EXPECT_TRUE(rate_limit_json_field->value.is_text());
    const auto *usage_json_field = root.as_object().find("usage_json");
    ASSERT_NE(usage_json_field, nullptr);
    EXPECT_TRUE(usage_json_field->value.is_object());
    EXPECT_EQ(root.as_object().find("request"), nullptr);
    EXPECT_EQ(root.as_object().find("provider_attempts"), nullptr);

    fiber::mem::BufPool attempts_pool;
    fiber::json::JsonParser attempts_parser;
    const std::string_view attempts_text = attempts_json_field->value.as_text();
    ASSERT_TRUE(attempts_parser.feed(attempts_text.data(), attempts_text.size()));
    attempts_parser.finish();
    fiber::json::JsonAny parsed_attempts;
    ASSERT_EQ(fiber::json::parse_document(attempts_parser, attempts_pool, parsed_attempts,
                                          [](fiber::json::JsonParser &value_parser, fiber::mem::BufPool &value_pool,
                                             fiber::json::JsonAny &value) noexcept {
                                              return fiber::json::parse_any(value_parser, value_pool, value);
                                          }),
              fiber::json::ParseStatus::Done);
    ASSERT_TRUE(parsed_attempts.is_array());
    ASSERT_EQ(parsed_attempts.as_array().size(), 1u);

    EXPECT_NE(audit_json.find("answer briefly"), std::string_view::npos);
    EXPECT_NE(audit_json.find("weather in Paris"), std::string_view::npos);
    EXPECT_NE(audit_json.find("look up weather"), std::string_view::npos);
    EXPECT_NE(audit_json.find("weather is sunny"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"(\"city\":\"Paris\")"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("attempts_json":"[{\")"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("schema_version":5)"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("requested_model":"logical","client_protocol":"openai")"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("content_type":"json_text","stream":false,"message_count":2,"tool_count":1)"),
              std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("request_json":"{)"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("response_json":"weather is sunny")"), std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("usage_json":{"promptTokens":8,"completionTokens":6,"total_tokens":14})"),
              std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("user_agent":"audit-client/1.0","host":"127.0.0.1","real_ip":"203.0.113.9")"),
              std::string_view::npos);
    EXPECT_NE(audit_json.find(R"("error_json":"")"), std::string_view::npos);
    const std::size_t secret_url = audit_json.find("SECRET_URL");
    ASSERT_NE(secret_url, std::string_view::npos);
    EXPECT_EQ(audit_json.find("SECRET_URL", secret_url + 1), std::string_view::npos);
    const std::size_t secret_base64 = audit_json.find("SECRET_BASE64");
    ASSERT_NE(secret_base64, std::string_view::npos);
    EXPECT_EQ(audit_json.find("SECRET_BASE64", secret_base64 + 1), std::string_view::npos);
}

TEST(LlmProxyIntegrationTest, AggregatesStreamedContentIntoTheSameOutputField) {
    FixtureHarness fixture({
            MockReply{
                    .status = 200,
                    .content_type = "text/event-stream",
                    .chunks =
                            {
                                    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"hello "
                                    "\"}}]}\n\n",
                                    "data: "
                                    "{\"choices\":[{\"delta\":{\"content\":\"stream\"},\"finish_reason\":\"stop\"}]}"
                                    "\n\n"
                                    "data: [DONE]\n\n",
                            },
                    .stream = true,
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse response =
            post_json(fixture.entry_port(), token,
                      R"({"model":"logical","stream":true,"messages":[{"role":"user","content":"say hello"}]})");

    ASSERT_EQ(response.system_error, 0);
    ASSERT_EQ(response.status, 200);
    ASSERT_TRUE(response.complete);
    ASSERT_TRUE(fixture.wait_for_audit_records(1));
    const std::string audit = fixture.audit_contents();
    ASSERT_FALSE(audit.empty());
    EXPECT_EQ(audit.find('\n'), audit.size() - 1);
    EXPECT_NE(audit.find(R"("content_type":"json_text","stream":true,"message_count":1,"tool_count":0)"),
              std::string::npos);
    EXPECT_NE(audit.find(R"("response_json":"hello stream","finish_reason":"stop")"), std::string::npos);
    EXPECT_NE(
            audit.find(
                    R"("output_role":"assistant","output_complete":true,"output_canonical_complete":true,"output_event_count":2)"),
            std::string::npos);
}

TEST(LlmProxyIntegrationTest, PreservesLargeUtf8OutputWithoutTruncation) {
    std::string content;
    content.reserve(90 * 1024);
    for (std::size_t index = 0; index < 30000; ++index) {
        content.append("你");
    }
    content.append("END");
    const std::string provider_body =
            R"({"choices":[{"message":{"role":"assistant","content":")" + content + R"("},"finish_reason":"stop"}]})";
    FixtureHarness fixture({
            MockReply{
                    .status = 200,
                    .body = provider_body,
            },
    });
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse response =
            post_json(fixture.entry_port(), token, R"({"model":"logical","messages":[{"role":"user","content":"x"}]})");

    ASSERT_EQ(response.system_error, 0);
    ASSERT_EQ(response.status, 200);
    ASSERT_TRUE(response.complete);
    ASSERT_TRUE(fixture.wait_for_audit_records(1));
    const std::string audit = fixture.audit_contents();
    EXPECT_NE(audit.find(content), std::string::npos);
    EXPECT_NE(audit.find(R"("output_canonical_complete":true)"), std::string::npos);
    EXPECT_EQ(audit.find("\"truncated\""), std::string::npos);
}

TEST(LlmProxyIntegrationTest, AuditGenerationFailureDoesNotChangeTheResponse) {
    FixtureHarness fixture(
            {
                    MockReply{
                            .status = 200,
                            .body = R"({"id":"ok","choices":[{"message":{"role":"assistant","content":"ok"}}]})",
                    },
            },
            false, false, 256);
    ASSERT_TRUE(fixture.valid());
    const std::string token = issue_token();
    ASSERT_FALSE(token.empty());

    const RawHttpResponse response =
            post_json(fixture.entry_port(), token, R"({"model":"logical","messages":[{"role":"user","content":"x"}]})");

    ASSERT_EQ(response.system_error, 0);
    ASSERT_EQ(response.status, 200);
    ASSERT_TRUE(response.complete);
    EXPECT_EQ(fixture.audit_stats().written_records, 0u);
    EXPECT_TRUE(fixture.audit_contents().empty());
    auto metrics = fixture.settlement_metrics();
    ASSERT_TRUE(metrics);
    EXPECT_NE(metrics->find("ai_server_audit_generation_failures_total 1"), std::string::npos);
}

} // namespace
