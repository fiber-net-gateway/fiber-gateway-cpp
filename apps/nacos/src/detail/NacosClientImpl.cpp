#include "NacosClientImpl.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

#include <async/Timeout.h>
#include <common/Assert.h>
#include <common/json/JsonParse.h>
#include <common/json/JsonParser.h>
#include <common/util/UrlForm.h>
#include <fiber/nacos/dto/JsonCodec.h>
#include <http/ClientHttp1Exchange.h>
#include <http/Http1ClientConnection.h>
#include <http/HttpCommon.h>
#include <http/HttpHeaders.h>
#include <net/SocketAddress.h>

namespace fiber::nacos::detail {
namespace {

constexpr std::string_view kLoginPath = "/v1/auth/users/login";
constexpr std::chrono::seconds kRefreshFailureDelay{5};

http::Http1ClientConnectionOptions make_connection_options(const NacosClientConfig &config, std::size_t server_index) {
    http::Http1ClientConnectionOptions result;
    result.peer_addr = net::SocketAddress(config.server_ips()[server_index], config.http_port());
    return result;
}

std::string make_host_header(const NacosClientConfig &config, std::size_t server_index) {
    const net::IpAddress &ip = config.server_ips()[server_index];
    std::string host;
    if (ip.is_v6()) {
        host.push_back('[');
        host.append(ip.to_string());
        host.push_back(']');
    } else {
        host = ip.to_string();
    }
    host.push_back(':');
    host.append(std::to_string(config.http_port()));
    return host;
}

std::string make_login_target(const NacosClientConfig &config) {
    std::string target = config.context_path();
    if (target == "/") {
        target.clear();
    }
    target.append(kLoginPath);
    return target;
}

std::chrono::seconds refresh_delay(std::int64_t token_ttl) noexcept {
    return std::chrono::seconds(std::max<std::int64_t>(kRefreshFailureDelay.count(), token_ttl * 9 / 10));
}

json::ParseStatus parse_auth_token_response(json::JsonParser &parser, mem::BufPool &pool,
                                            dto::resp::AuthTokenResponse &out) noexcept {
    return dto::parse_json(parser, pool, out);
}

void append_chunk(mem::IoBufChain &chunk, std::string &out) {
    while (chunk.readable_bytes() > 0) {
        mem::IoBuf *front = chunk.first_readable();
        if (!front) {
            return;
        }
        const std::size_t readable = front->readable();
        out.append(reinterpret_cast<const char *>(front->readable_data()), readable);
        chunk.consume_and_compact(readable);
    }
}

} // namespace

NacosClientImpl::NacosClientImpl(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options) :
    loop_(&loop), config_(std::move(config)), options_(std::move(options)),
    config_service_(loop, config_, options_, auth_watch_.subscribe()) {
    shutdown_publisher_ = shutdown_watch_.acquire_publisher();
    auth_publisher_ = auth_watch_.acquire_publisher();
    FIBER_ASSERT(shutdown_publisher_.has_value());
    FIBER_ASSERT(auth_publisher_.has_value());
}

NacosClientImpl::~NacosClientImpl() {
    FIBER_ASSERT(state_ == NacosClientState::Created || state_ == NacosClientState::Stopped);
    FIBER_ASSERT(task_group_.empty());
}

bool NacosClientImpl::valid_options(const NacosClientOptions &options) noexcept {
    return options.connect_timeout > std::chrono::milliseconds::zero() &&
           options.request_timeout > std::chrono::milliseconds::zero() && options.max_auth_response_bytes > 0 &&
           options.retry_initial_delay > std::chrono::milliseconds::zero() &&
           options.retry_max_delay >= options.retry_initial_delay && ConfigServiceImpl::valid_options(options);
}

common::IoResult<void> NacosClientImpl::start() noexcept {
    if (!loop_->in_loop()) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    if (state_ != NacosClientState::Created) {
        return std::unexpected(common::IoErr::Already);
    }
    state_ = NacosClientState::Running;
    task_group_.add();
    async::spawn(*loop_, [this]() { return run_auth(); });
    task_group_.add();
    async::spawn(*loop_, [this]() { return run_config(); });
    return {};
}

async::Task<void> NacosClientImpl::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());

    if (state_ == NacosClientState::Stopped) {
        co_return;
    }
    if (state_ == NacosClientState::Created || state_ == NacosClientState::Running) {
        state_ = NacosClientState::Stopping;
        config_service_.shutdown();
        shutdown_publisher_->publish(true);
        publish_auth(NacosAuthAccess{.kind = NacosAuthAccessKind::Stopped});
    }

    co_await task_group_.join();
    state_ = NacosClientState::Stopped;
}

async::Watch<NacosAuthAccess>::Subscriber NacosClientImpl::subscribe_auth() { return auth_watch_.subscribe(); }

async::Watch<bool>::Subscriber NacosClientImpl::subscribe_shutdown() { return shutdown_watch_.subscribe(); }

void NacosClientImpl::end_task() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    task_group_.done();
}

void NacosClientImpl::publish_auth(NacosAuthAccess auth_access) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT((auth_access.kind == NacosAuthAccessKind::Present) == !auth_access.access_token.empty());
    auth_publisher_->publish(std::move(auth_access));
}

async::DetachedTask NacosClientImpl::run_auth() noexcept {
    auto shutdown_subscriber = subscribe_shutdown();
    const auto initial_shutdown = shutdown_subscriber.current();
    FIBER_ASSERT(initial_shutdown.value != nullptr);
    std::uint64_t shutdown_version = initial_shutdown.version;

    if (config_.username().empty()) {
        FIBER_ASSERT(config_.password().empty());
        publish_auth(NacosAuthAccess{.kind = NacosAuthAccessKind::NotConfigured});
        end_task();
        co_return;
    }

    std::string auth_body;
    auth_body.append("username=");
    util::form_encode(config_.username(), auth_body);
    auth_body.append("&password=");
    util::form_encode(config_.password(), auth_body);

    const std::string target = make_login_target(config_);
    std::string access_token;
    std::size_t preferred_server_index = 0;
    auto retry_delay = options_.retry_initial_delay;
    auto next_attempt_at = event::EventLoop::current().now();
    bool initial_failure_published = false;

    while (running()) {
        auto now = event::EventLoop::current().now();
        if (now < next_attempt_at) {
            auto wait_result = co_await async::timeout_for(
                    [&shutdown_subscriber, shutdown_version]() { return shutdown_subscriber.next(shutdown_version); },
                    next_attempt_at - now);
            if (wait_result) {
                shutdown_version = wait_result->version;
                FIBER_ASSERT(wait_result->value != nullptr);
                if (*wait_result->value) {
                    break;
                }
            } else {
                FIBER_ASSERT(wait_result.error() == common::IoErr::TimedOut);
            }
            continue;
        }

        AuthLoginSuccess success;
        std::size_t successful_server_index = 0;
        bool succeeded = false;

        const std::size_t server_count = config_.server_ips().size();
        for (std::size_t offset = 0; offset < server_count && running(); ++offset) {
            const std::size_t server_index = (preferred_server_index + offset) % server_count;
            auto result = co_await login(server_index, target, auth_body);
            if (!running()) {
                break;
            }
            if (result) {
                success = std::move(*result);
                successful_server_index = server_index;
                succeeded = true;
                break;
            }
        }

        if (!running()) {
            break;
        }

        now = event::EventLoop::current().now();
        if (succeeded) {
            const auto max_ttl =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::time_point::max() - now)
                            .count();
            if (success.token_ttl > max_ttl) {
                succeeded = false;
            }
        }

        if (succeeded) {
            preferred_server_index = successful_server_index;
            retry_delay = options_.retry_initial_delay;
            access_token = std::move(success.access_token);
            publish_auth(NacosAuthAccess{
                    .kind = NacosAuthAccessKind::Present,
                    .access_token = access_token,
            });
            next_attempt_at = now + refresh_delay(success.token_ttl);
            continue;
        }

        if (access_token.empty()) {
            if (!initial_failure_published) {
                publish_auth(NacosAuthAccess{.kind = NacosAuthAccessKind::InitialFailed});
                initial_failure_published = true;
            }
            next_attempt_at = now + retry_delay;
            if (retry_delay < options_.retry_max_delay) {
                retry_delay = retry_delay > options_.retry_max_delay / 2 ? options_.retry_max_delay : retry_delay * 2;
            }
        } else {
            next_attempt_at = now + kRefreshFailureDelay;
        }
    }

    end_task();
    co_return;
}

async::DetachedTask NacosClientImpl::run_config() noexcept {
    co_await config_service_.run();
    end_task();
}

async::Task<std::expected<NacosClientImpl::AuthLoginSuccess, common::IoErr>>
NacosClientImpl::login(std::size_t server_index, std::string_view target, std::string_view auth_body) noexcept {
    if (!running()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    http::Http1ClientConnection connection(*loop_, make_connection_options(config_, server_index));
    auto connect_result = co_await connection.connect(options_.connect_timeout);
    if (!running()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }

    mem::BufPool pool;
    http::ClientHttp1Exchange exchange(connection, pool);
    http::HttpHeaders headers(pool);
    const std::string host_header = make_host_header(config_, server_index);
    if (!headers.add_view("host", host_header) ||
        !headers.add_view("content-type", "application/x-www-form-urlencoded") ||
        !headers.add_view("accept", "application/json") || !headers.add_view("connection", "close")) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    http::Http1RequestHead request;
    request.method = http::HttpMethod::Post;
    request.target = target;
    request.headers = &headers;
    request.body = http::HttpBodySpec::ContentLength(auth_body.size());

    auto send_header_result = co_await exchange.send_header(request, auth_body.empty(), options_.request_timeout);
    if (!running()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if (!send_header_result) {
        co_return std::unexpected(send_header_result.error());
    }

    if (!auth_body.empty()) {
        auto send_body_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(auth_body.data()),
                                                             auth_body.size(), true, options_.request_timeout);
        if (!running()) {
            co_return std::unexpected(common::IoErr::Canceled);
        }
        if (!send_body_result) {
            co_return std::unexpected(send_body_result.error());
        }
    }

    const http::Http1ResponseHead *response = nullptr;
    for (;;) {
        auto header_result = co_await exchange.read_header(options_.request_timeout);
        if (!running()) {
            co_return std::unexpected(common::IoErr::Canceled);
        }
        if (!header_result) {
            co_return std::unexpected(header_result.error());
        }
        response = *header_result;
        if (!response->is_informational()) {
            break;
        }
    }

    if (response->status_code != 200) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::string response_body;
    response_body.reserve(std::min<std::size_t>(options_.max_auth_response_bytes, 4096));
    for (;;) {
        const std::size_t remaining = options_.max_auth_response_bytes - response_body.size();
        const std::size_t read_limit = remaining == std::numeric_limits<std::size_t>::max() ? remaining : remaining + 1;
        auto body_result = co_await exchange.read_body(read_limit, options_.request_timeout);
        if (!running()) {
            co_return std::unexpected(common::IoErr::Canceled);
        }
        if (!body_result) {
            co_return std::unexpected(body_result.error());
        }
        if (body_result->readable_bytes() > remaining) {
            co_return std::unexpected(common::IoErr::MessageTooLarge);
        }
        append_chunk(*body_result, response_body);
        if (body_result->complete()) {
            break;
        }
    }

    json::JsonParser parser;
    if (!parser.feed(response_body.data(), response_body.size())) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    parser.finish();
    dto::resp::AuthTokenResponse token_response;
    if (json::parse_document<parse_auth_token_response>(parser, pool, token_response) != json::ParseStatus::Done) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!token_response.access_token.is_present() || token_response.access_token.value().empty()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (token_response.token_ttl <= 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    AuthLoginSuccess success;
    success.access_token.assign(token_response.access_token.value());
    success.token_ttl = token_response.token_ttl;
    co_return success;
}

} // namespace fiber::nacos::detail
