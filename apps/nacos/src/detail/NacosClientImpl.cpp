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

constexpr std::string_view kV3LoginPath = "/v3/auth/user/login";
constexpr std::string_view kLegacyV1LoginPath = "/v1/auth/users/login";

bool endpoint_not_supported(const NacosAuthError &error) noexcept {
    return error.code == NacosAuthErrorCode::HttpStatus && (error.http_status == 404 || error.http_status == 405);
}

NacosAuthError canceled_error(std::size_t server_index = 0) noexcept {
    return NacosAuthError{
            .code = NacosAuthErrorCode::Canceled,
            .io_error = common::IoErr::Canceled,
            .server_index = server_index,
    };
}

NacosAuthError token_expired_error(std::size_t server_index) noexcept {
    return NacosAuthError{
            .code = NacosAuthErrorCode::TokenExpired,
            .server_index = server_index,
    };
}

NacosAuthError make_io_error(common::IoErr error, std::size_t server_index,
                             std::chrono::steady_clock::time_point deadline,
                             std::chrono::steady_clock::time_point now) noexcept {
    if (deadline != std::chrono::steady_clock::time_point::max() && now >= deadline) {
        return token_expired_error(server_index);
    }
    return NacosAuthError{
            .code = error == common::IoErr::Canceled ? NacosAuthErrorCode::Canceled : NacosAuthErrorCode::Io,
            .io_error = error,
            .server_index = server_index,
    };
}

NacosAuthError make_error(NacosAuthErrorCode code, std::size_t server_index) noexcept {
    return NacosAuthError{.code = code, .server_index = server_index};
}

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

std::string make_login_target(const NacosClientConfig &config, NacosAuthApiVersion version) {
    std::string target = config.context_path();
    if (target == "/") {
        target.clear();
    }
    target.append(version == NacosAuthApiVersion::LegacyV1 ? kLegacyV1LoginPath : kV3LoginPath);
    return target;
}

std::optional<std::chrono::milliseconds> bounded_timeout(std::chrono::milliseconds configured,
                                                         std::chrono::steady_clock::time_point deadline,
                                                         std::chrono::steady_clock::time_point now) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return configured;
    }
    if (now >= deadline) {
        return std::nullopt;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return std::min(configured, remaining);
}

bool token_valid(const NacosAuthSnapshot &snapshot, std::chrono::steady_clock::time_point now) noexcept {
    return snapshot.state == NacosAuthState::Ready && now < snapshot.expires_at && !snapshot.access_token.empty();
}

std::chrono::steady_clock::duration refresh_delay(std::int64_t token_ttl, const NacosClientOptions &options) noexcept {
    const auto ttl = std::chrono::seconds(token_ttl);
    auto delay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(
            static_cast<double>(token_ttl) * static_cast<double>(options.refresh_percent) / 100.0));
    const auto minimum = std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.min_refresh_delay);
    if (delay < minimum) {
        delay = minimum;
    }
    if (delay >= ttl) {
        delay = ttl / 2;
    }
    if (delay <= std::chrono::steady_clock::duration::zero()) {
        delay = std::chrono::milliseconds(1);
    }
    return delay;
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
    loop_(&loop), config_(std::move(config)), options_(std::move(options)), grpc_connection_(loop, config_, options_),
    config_service_(loop, config_, options_, grpc_connection_) {
    shutdown_publisher_ = shutdown_watch_.acquire_publisher();
    auth_publisher_ = auth_watch_.acquire_publisher();
    FIBER_ASSERT(shutdown_publisher_.has_value());
    FIBER_ASSERT(auth_publisher_.has_value());
}

NacosClientImpl::~NacosClientImpl() {
    FIBER_ASSERT(state_ == NacosClientState::Created || state_ == NacosClientState::Stopped);
    FIBER_ASSERT(task_group_.empty());
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
    async::spawn(*loop_, [this]() { return run_grpc(); });
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
        grpc_connection_.shutdown();
        shutdown_publisher_->publish(true);
    }

    co_await task_group_.join();
    if (state_ != NacosClientState::Stopped) {
        publish_stopped();
        state_ = NacosClientState::Stopped;
    }
}

async::Watch<NacosAuthSnapshot>::Subscriber NacosClientImpl::subscribe_auth() { return auth_watch_.subscribe(); }

async::Watch<bool>::Subscriber NacosClientImpl::subscribe_shutdown() { return shutdown_watch_.subscribe(); }

void NacosClientImpl::end_task() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    task_group_.done();
}

void NacosClientImpl::publish_auth(NacosAuthSnapshot snapshot) {
    FIBER_ASSERT(loop_->in_loop());
    grpc_connection_.notify_auth(snapshot);
    auth_publisher_->publish(std::move(snapshot));
}

void NacosClientImpl::publish_stopped() {
    FIBER_ASSERT(loop_->in_loop());
    auto subscriber = auth_watch_.subscribe();
    auto current = subscriber.current();
    NacosAuthSnapshot snapshot;
    if (current.value) {
        snapshot = *current.value;
    }
    snapshot.state = NacosAuthState::Stopped;
    snapshot.access_token.clear();
    snapshot.expires_at = {};
    snapshot.last_error = {};
    publish_auth(std::move(snapshot));
}

async::DetachedTask NacosClientImpl::run_auth() noexcept {
    auto shutdown_subscriber = subscribe_shutdown();
    const auto initial_shutdown = shutdown_subscriber.current();
    FIBER_ASSERT(initial_shutdown.value != nullptr);
    std::uint64_t shutdown_version = initial_shutdown.version;

    std::string auth_body;
    auth_body.append("username=");
    util::form_encode(config_.username(), auth_body);
    auth_body.append("&password=");
    util::form_encode(config_.password(), auth_body);

    const std::string v3_target = make_login_target(config_, NacosAuthApiVersion::V3);
    const std::string legacy_target = make_login_target(config_, NacosAuthApiVersion::LegacyV1);

    NacosAuthSnapshot snapshot;
    std::optional<NacosAuthApiVersion> resolved_auth_api;
    std::size_t preferred_server_index = 0;
    auto retry_delay = options_.retry_initial_delay;
    auto next_attempt_at = event::EventLoop::current().now();

    while (running()) {
        auto now = event::EventLoop::current().now();
        if (snapshot.state == NacosAuthState::Ready && now >= snapshot.expires_at) {
            snapshot.state = NacosAuthState::Unavailable;
            snapshot.access_token.clear();
            snapshot.expires_at = {};
            snapshot.last_error = NacosAuthError{.code = NacosAuthErrorCode::TokenExpired};
            publish_auth(snapshot);
        }

        auto wake_at = next_attempt_at;
        if (snapshot.state == NacosAuthState::Ready && snapshot.expires_at < wake_at) {
            wake_at = snapshot.expires_at;
        }
        if (now < wake_at) {
            auto wait_result = co_await async::timeout_for(
                    [&shutdown_subscriber, shutdown_version]() { return shutdown_subscriber.next(shutdown_version); },
                    wake_at - now);
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

        const auto request_deadline =
                token_valid(snapshot, now) ? snapshot.expires_at : std::chrono::steady_clock::time_point::max();
        NacosAuthError last_error{
                .code = NacosAuthErrorCode::Io,
                .io_error = common::IoErr::NotFound,
        };
        AuthLoginSuccess success;
        std::size_t successful_server_index = 0;
        bool succeeded = false;
        bool deadline_exhausted = false;

        const std::size_t server_count = config_.server_ips().size();
        for (std::size_t offset = 0; offset < server_count && running(); ++offset) {
            const std::size_t server_index = (preferred_server_index + offset) % server_count;

            if (resolved_auth_api) {
                const std::string_view target =
                        *resolved_auth_api == NacosAuthApiVersion::LegacyV1 ? legacy_target : v3_target;
                auto result = co_await login(server_index, target, auth_body, request_deadline);
                if (!running()) {
                    break;
                }
                if (result) {
                    success = std::move(*result);
                    successful_server_index = server_index;
                    succeeded = true;
                    break;
                }
                last_error = result.error();
                deadline_exhausted = last_error.code == NacosAuthErrorCode::TokenExpired;
                if (deadline_exhausted) {
                    break;
                }
                continue;
            }

            switch (config_.auth_api_version()) {
                case NacosAuthApiVersion::V3: {
                    auto result = co_await login(server_index, v3_target, auth_body, request_deadline);
                    if (!running()) {
                        break;
                    }
                    if (result) {
                        success = std::move(*result);
                        successful_server_index = server_index;
                        succeeded = true;
                    } else {
                        last_error = result.error();
                        deadline_exhausted = last_error.code == NacosAuthErrorCode::TokenExpired;
                    }
                    break;
                }
                case NacosAuthApiVersion::LegacyV1: {
                    auto result = co_await login(server_index, legacy_target, auth_body, request_deadline);
                    if (!running()) {
                        break;
                    }
                    if (result) {
                        success = std::move(*result);
                        successful_server_index = server_index;
                        succeeded = true;
                    } else {
                        last_error = result.error();
                        deadline_exhausted = last_error.code == NacosAuthErrorCode::TokenExpired;
                    }
                    break;
                }
                case NacosAuthApiVersion::Auto: {
                    auto result = co_await login(server_index, v3_target, auth_body, request_deadline);
                    if (!running()) {
                        break;
                    }
                    if (result) {
                        resolved_auth_api = NacosAuthApiVersion::V3;
                        success = std::move(*result);
                        successful_server_index = server_index;
                        succeeded = true;
                        break;
                    }
                    last_error = result.error();
                    deadline_exhausted = last_error.code == NacosAuthErrorCode::TokenExpired;
                    if (deadline_exhausted || !endpoint_not_supported(last_error)) {
                        break;
                    }

                    result = co_await login(server_index, legacy_target, auth_body, request_deadline);
                    if (!running()) {
                        break;
                    }
                    if (result) {
                        resolved_auth_api = NacosAuthApiVersion::LegacyV1;
                        success = std::move(*result);
                        successful_server_index = server_index;
                        succeeded = true;
                    } else {
                        last_error = result.error();
                        deadline_exhausted = last_error.code == NacosAuthErrorCode::TokenExpired;
                    }
                    break;
                }
            }

            if (succeeded || deadline_exhausted) {
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
                last_error = NacosAuthError{
                        .code = NacosAuthErrorCode::InvalidTokenTtl,
                        .server_index = successful_server_index,
                };
            }
        }

        if (succeeded) {
            preferred_server_index = successful_server_index;
            retry_delay = options_.retry_initial_delay;
            snapshot.state = NacosAuthState::Ready;
            snapshot.access_token = std::move(success.access_token);
            snapshot.username = std::move(success.username);
            snapshot.global_admin = success.global_admin;
            ++snapshot.generation;
            snapshot.expires_at = now + std::chrono::seconds(success.token_ttl);
            snapshot.last_error = {};
            publish_auth(snapshot);
            next_attempt_at = now + refresh_delay(success.token_ttl, options_);
            continue;
        }

        if (snapshot.state == NacosAuthState::Ready && now >= snapshot.expires_at) {
            snapshot.state = NacosAuthState::Unavailable;
            snapshot.access_token.clear();
            snapshot.expires_at = {};
            snapshot.last_error = NacosAuthError{.code = NacosAuthErrorCode::TokenExpired};
        } else {
            if (!token_valid(snapshot, now)) {
                snapshot.state = NacosAuthState::Unavailable;
                snapshot.access_token.clear();
                snapshot.expires_at = {};
            }
            snapshot.last_error = last_error;
        }
        publish_auth(snapshot);

        next_attempt_at = now + retry_delay;
        if (retry_delay < options_.retry_max_delay) {
            retry_delay = retry_delay > options_.retry_max_delay / 2 ? options_.retry_max_delay : retry_delay * 2;
        }
    }

    end_task();
    co_return;
}

async::DetachedTask NacosClientImpl::run_grpc() noexcept {
    co_await grpc_connection_.run();
    end_task();
}

async::DetachedTask NacosClientImpl::run_config() noexcept {
    co_await config_service_.run();
    end_task();
}

async::Task<std::expected<NacosClientImpl::AuthLoginSuccess, NacosAuthError>>
NacosClientImpl::login(std::size_t server_index, std::string_view target, std::string_view auth_body,
                       std::chrono::steady_clock::time_point deadline) noexcept {
    if (!running()) {
        co_return std::unexpected(canceled_error(server_index));
    }

    auto connect_timeout = bounded_timeout(options_.connect_timeout, deadline, event::EventLoop::current().now());
    if (!connect_timeout) {
        co_return std::unexpected(token_expired_error(server_index));
    }

    http::Http1ClientConnection connection(*loop_, make_connection_options(config_, server_index));
    auto connect_result = co_await connection.connect(*connect_timeout);
    if (!running()) {
        co_return std::unexpected(canceled_error(server_index));
    }
    if (!connect_result) {
        co_return std::unexpected(
                make_io_error(connect_result.error(), server_index, deadline, event::EventLoop::current().now()));
    }

    mem::BufPool pool;
    http::ClientHttp1Exchange exchange(connection, pool);
    http::HttpHeaders headers(pool);
    const std::string host_header = make_host_header(config_, server_index);
    if (!headers.add_view("host", host_header) ||
        !headers.add_view("content-type", "application/x-www-form-urlencoded") ||
        !headers.add_view("accept", "application/json") || !headers.add_view("connection", "close")) {
        co_return std::unexpected(
                make_io_error(common::IoErr::NoMem, server_index, deadline, event::EventLoop::current().now()));
    }

    http::Http1RequestHead request;
    request.method = http::HttpMethod::Post;
    request.target = target;
    request.headers = &headers;
    request.body = http::HttpBodySpec::ContentLength(auth_body.size());

    auto request_timeout = bounded_timeout(options_.request_timeout, deadline, event::EventLoop::current().now());
    if (!request_timeout) {
        co_return std::unexpected(token_expired_error(server_index));
    }
    auto send_header_result = co_await exchange.send_header(request, auth_body.empty(), *request_timeout);
    if (!running()) {
        co_return std::unexpected(canceled_error(server_index));
    }
    if (!send_header_result) {
        co_return std::unexpected(
                make_io_error(send_header_result.error(), server_index, deadline, event::EventLoop::current().now()));
    }

    if (!auth_body.empty()) {
        request_timeout = bounded_timeout(options_.request_timeout, deadline, event::EventLoop::current().now());
        if (!request_timeout) {
            co_return std::unexpected(token_expired_error(server_index));
        }
        auto send_body_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(auth_body.data()),
                                                             auth_body.size(), true, *request_timeout);
        if (!running()) {
            co_return std::unexpected(canceled_error(server_index));
        }
        if (!send_body_result) {
            co_return std::unexpected(
                    make_io_error(send_body_result.error(), server_index, deadline, event::EventLoop::current().now()));
        }
    }

    const http::Http1ResponseHead *response = nullptr;
    for (;;) {
        request_timeout = bounded_timeout(options_.request_timeout, deadline, event::EventLoop::current().now());
        if (!request_timeout) {
            co_return std::unexpected(token_expired_error(server_index));
        }
        auto header_result = co_await exchange.read_header(*request_timeout);
        if (!running()) {
            co_return std::unexpected(canceled_error(server_index));
        }
        if (!header_result) {
            co_return std::unexpected(
                    make_io_error(header_result.error(), server_index, deadline, event::EventLoop::current().now()));
        }
        response = *header_result;
        if (!response->is_informational()) {
            break;
        }
    }

    if (response->status_code != 200) {
        NacosAuthError error = make_error(NacosAuthErrorCode::HttpStatus, server_index);
        error.http_status = response->status_code;
        co_return std::unexpected(error);
    }

    std::string response_body;
    response_body.reserve(std::min<std::size_t>(options_.max_auth_response_bytes, 4096));
    for (;;) {
        const std::size_t remaining = options_.max_auth_response_bytes - response_body.size();
        const std::size_t read_limit = remaining == std::numeric_limits<std::size_t>::max() ? remaining : remaining + 1;
        request_timeout = bounded_timeout(options_.request_timeout, deadline, event::EventLoop::current().now());
        if (!request_timeout) {
            co_return std::unexpected(token_expired_error(server_index));
        }
        auto body_result = co_await exchange.read_body(read_limit, *request_timeout);
        if (!running()) {
            co_return std::unexpected(canceled_error(server_index));
        }
        if (!body_result) {
            co_return std::unexpected(
                    make_io_error(body_result.error(), server_index, deadline, event::EventLoop::current().now()));
        }
        if (body_result->readable_bytes() > remaining) {
            co_return std::unexpected(make_error(NacosAuthErrorCode::ResponseTooLarge, server_index));
        }
        append_chunk(*body_result, response_body);
        if (body_result->complete()) {
            break;
        }
    }

    json::JsonParser parser;
    if (!parser.feed(response_body.data(), response_body.size())) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::InvalidJson, server_index));
    }
    parser.finish();
    dto::resp::AuthTokenResponse token_response;
    if (json::parse_document<parse_auth_token_response>(parser, pool, token_response) != json::ParseStatus::Done) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::InvalidJson, server_index));
    }
    if (!token_response.access_token.is_present() || token_response.access_token.value().empty()) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::MissingAccessToken, server_index));
    }
    if (token_response.token_ttl <= 0) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::InvalidTokenTtl, server_index));
    }

    AuthLoginSuccess success;
    success.access_token.assign(token_response.access_token.value());
    if (token_response.username.is_present()) {
        success.username.assign(token_response.username.value());
    } else {
        success.username = config_.username();
    }
    success.token_ttl = token_response.token_ttl;
    success.global_admin = token_response.global_admin;
    co_return success;
}

} // namespace fiber::nacos::detail
