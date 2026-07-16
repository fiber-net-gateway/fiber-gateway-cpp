#include "NacosAuthenticator.h"

#include <algorithm>
#include <chrono>
#include <expected>
#include <utility>

#include <common/Assert.h>
#include <common/util/UrlForm.h>

#include "../detail/NacosClientImpl.h"
#include "AuthHttpOperation.h"

namespace fiber::nacos::detail {
namespace {

constexpr std::string_view kV3LoginPath = "/v3/auth/user/login";
constexpr std::string_view kLegacyV1LoginPath = "/v1/auth/users/login";

bool endpoint_not_supported(const NacosAuthError &error) noexcept {
    return error.code == NacosAuthErrorCode::HttpStatus && (error.http_status == 404 || error.http_status == 405);
}

NacosAuthError canceled_error() noexcept {
    return NacosAuthError{
            .code = NacosAuthErrorCode::Canceled,
            .io_error = common::IoErr::Canceled,
    };
}

} // namespace

NacosAuthenticator::NacosAuthenticator(NacosClientImpl &client) :
    client_(&client), shutdown_subscriber_(client.subscribe_shutdown()),
    retry_delay_(client.options().retry_initial_delay) {
    auth_body_.append("username=");
    util::form_encode(client.config().username(), auth_body_);
    auth_body_.append("&password=");
    util::form_encode(client.config().password(), auth_body_);
}

NacosAuthenticator::~NacosAuthenticator() {
    FIBER_ASSERT(!attempt_timer_.is_in_heap());
    FIBER_ASSERT(!expiry_timer_.is_in_heap());
    FIBER_ASSERT(!attempt_active_);
    FIBER_ASSERT(!active_operation_);
}

NacosAuthenticator::TaskDoneGuard::~TaskDoneGuard() {
    if (client_) {
        client_->end_task();
    }
}

void NacosAuthenticator::start() noexcept {
    FIBER_ASSERT(client_->loop().in_loop());
    stopping_ = false;
    schedule_attempt(client_->loop().now());
}

void NacosAuthenticator::stop() noexcept {
    FIBER_ASSERT(client_->loop().in_loop());
    if (stopping_) {
        return;
    }
    stopping_ = true;
    if (attempt_timer_.is_in_heap()) {
        client_->loop().cancel<NacosAuthenticator, &NacosAuthenticator::attempt_timer_>(*this);
    }
    if (expiry_timer_.is_in_heap()) {
        client_->loop().cancel<NacosAuthenticator, &NacosAuthenticator::expiry_timer_>(*this);
    }
    if (active_operation_) {
        std::shared_ptr<AuthHttpOperation> operation = active_operation_;
        operation->cancel();
    }
}

void NacosAuthenticator::publish_stopped() {
    FIBER_ASSERT(client_->loop().in_loop());
    snapshot_.state = NacosAuthState::Stopped;
    snapshot_.access_token.clear();
    snapshot_.expires_at = {};
    snapshot_.last_error = {};
    client_->publish_auth(snapshot_);
}

void NacosAuthenticator::schedule_attempt(std::chrono::steady_clock::time_point when) noexcept {
    FIBER_ASSERT(client_->loop().in_loop());
    if (shutdown_requested() || !client_->running()) {
        return;
    }
    if (attempt_timer_.is_in_heap()) {
        client_->loop().cancel<NacosAuthenticator, &NacosAuthenticator::attempt_timer_>(*this);
    }
    client_->loop()
            .post_at<NacosAuthenticator, &NacosAuthenticator::attempt_timer_, &NacosAuthenticator::on_attempt_timer>(
                    when, *this);
}

void NacosAuthenticator::start_attempt() noexcept {
    FIBER_ASSERT(client_->loop().in_loop());
    if (shutdown_requested() || attempt_active_ || !client_->try_begin_task()) {
        return;
    }
    attempt_active_ = true;
    async::spawn(client_->loop(), [this]() { return run_attempt_tracked(); });
}

async::DetachedTask NacosAuthenticator::run_attempt_tracked() noexcept {
    TaskDoneGuard guard(*client_);
    co_await run_attempt();
    active_operation_.reset();
    attempt_active_ = false;
}

async::Task<void> NacosAuthenticator::run_attempt() noexcept {
    const std::size_t server_count = client_->config().server_ips().size();
    NacosAuthError last_error{
            .code = NacosAuthErrorCode::Io,
            .io_error = common::IoErr::NotFound,
    };

    for (std::size_t offset = 0; offset < server_count; ++offset) {
        if (shutdown_requested()) {
            co_return;
        }
        const std::size_t server_index = (preferred_server_index_ + offset) % server_count;

        if (resolved_auth_api_) {
            auto result = co_await request(server_index, *resolved_auth_api_);
            if (result) {
                handle_success(std::move(*result), server_index);
                co_return;
            }
            last_error = result.error();
            continue;
        }

        switch (client_->config().auth_api_version()) {
            case NacosAuthApiVersion::V3: {
                auto result = co_await request(server_index, NacosAuthApiVersion::V3);
                if (result) {
                    handle_success(std::move(*result), server_index);
                    co_return;
                }
                last_error = result.error();
                break;
            }
            case NacosAuthApiVersion::LegacyV1: {
                auto result = co_await request(server_index, NacosAuthApiVersion::LegacyV1);
                if (result) {
                    handle_success(std::move(*result), server_index);
                    co_return;
                }
                last_error = result.error();
                break;
            }
            case NacosAuthApiVersion::Auto: {
                auto result = co_await request(server_index, NacosAuthApiVersion::V3);
                if (result) {
                    resolved_auth_api_ = NacosAuthApiVersion::V3;
                    handle_success(std::move(*result), server_index);
                    co_return;
                }
                last_error = result.error();
                if (!endpoint_not_supported(last_error) || shutdown_requested()) {
                    break;
                }
                result = co_await request(server_index, NacosAuthApiVersion::LegacyV1);
                if (result) {
                    resolved_auth_api_ = NacosAuthApiVersion::LegacyV1;
                    handle_success(std::move(*result), server_index);
                    co_return;
                }
                last_error = result.error();
                break;
            }
        }
    }

    if (!shutdown_requested()) {
        schedule_retry(last_error);
    }
}

async::Task<std::expected<AuthHttpSuccess, NacosAuthError>>
NacosAuthenticator::request(std::size_t server_index, NacosAuthApiVersion version) noexcept {
    if (shutdown_requested()) {
        co_return std::unexpected(canceled_error());
    }
    auto operation = std::make_shared<AuthHttpOperation>(client_->loop(), client_->config(), client_->options(),
                                                         server_index, make_target(version), auth_body_);
    active_operation_ = operation;
    auto result = co_await operation->run();
    if (active_operation_.get() == operation.get()) {
        active_operation_.reset();
    }
    if (shutdown_requested()) {
        co_return std::unexpected(canceled_error());
    }
    co_return result;
}

std::string NacosAuthenticator::make_target(NacosAuthApiVersion version) const {
    std::string target = client_->config().context_path();
    if (target == "/") {
        target.clear();
    }
    target.append(version == NacosAuthApiVersion::LegacyV1 ? kLegacyV1LoginPath : kV3LoginPath);
    return target;
}

bool NacosAuthenticator::token_valid(std::chrono::steady_clock::time_point now) const noexcept {
    return snapshot_.state == NacosAuthState::Ready && now < snapshot_.expires_at && !snapshot_.access_token.empty();
}

bool NacosAuthenticator::shutdown_requested() {
    const auto shutdown = shutdown_subscriber_.current();
    FIBER_ASSERT(shutdown.value != nullptr);
    return stopping_ || *shutdown.value;
}

void NacosAuthenticator::handle_success(AuthHttpSuccess success, std::size_t server_index) {
    const auto now = client_->loop().now();
    const auto max_ttl =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::time_point::max() - now)
                    .count();
    if (success.token_ttl <= 0 || success.token_ttl > max_ttl) {
        NacosAuthError error{
                .code = NacosAuthErrorCode::InvalidTokenTtl,
                .server_index = server_index,
        };
        schedule_retry(error);
        return;
    }

    const auto ttl = std::chrono::seconds(success.token_ttl);
    auto refresh_delay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(
            static_cast<double>(success.token_ttl) * static_cast<double>(client_->options().refresh_percent) / 100.0));
    const auto min_refresh =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(client_->options().min_refresh_delay);
    if (refresh_delay < min_refresh) {
        refresh_delay = min_refresh;
    }
    if (refresh_delay >= ttl) {
        refresh_delay = ttl / 2;
    }
    if (refresh_delay <= std::chrono::steady_clock::duration::zero()) {
        refresh_delay = std::chrono::milliseconds(1);
    }

    preferred_server_index_ = server_index;
    retry_delay_ = client_->options().retry_initial_delay;
    snapshot_.state = NacosAuthState::Ready;
    snapshot_.access_token = std::move(success.access_token);
    snapshot_.username = std::move(success.username);
    snapshot_.global_admin = success.global_admin;
    ++snapshot_.generation;
    snapshot_.expires_at = now + ttl;
    snapshot_.last_error = {};
    client_->publish_auth(snapshot_);

    if (expiry_timer_.is_in_heap()) {
        client_->loop().cancel<NacosAuthenticator, &NacosAuthenticator::expiry_timer_>(*this);
    }
    client_->loop()
            .post_at<NacosAuthenticator, &NacosAuthenticator::expiry_timer_, &NacosAuthenticator::on_expiry_timer>(
                    snapshot_.expires_at, *this);
    schedule_attempt(now + refresh_delay);
}

void NacosAuthenticator::publish_failure(const NacosAuthError &error) {
    const auto now = client_->loop().now();
    if (!token_valid(now)) {
        snapshot_.state = NacosAuthState::Unavailable;
        snapshot_.access_token.clear();
        snapshot_.expires_at = {};
    }
    snapshot_.last_error = error;
    client_->publish_auth(snapshot_);
}

void NacosAuthenticator::schedule_retry(const NacosAuthError &error) {
    publish_failure(error);
    schedule_attempt(client_->loop().now() + retry_delay_);
    const auto max_delay = client_->options().retry_max_delay;
    if (retry_delay_ < max_delay) {
        retry_delay_ = retry_delay_ > max_delay / 2 ? max_delay : retry_delay_ * 2;
    }
}

void NacosAuthenticator::publish_expired() {
    if (shutdown_requested() || snapshot_.state != NacosAuthState::Ready) {
        return;
    }
    snapshot_.state = NacosAuthState::Unavailable;
    snapshot_.access_token.clear();
    snapshot_.expires_at = {};
    snapshot_.last_error = NacosAuthError{.code = NacosAuthErrorCode::TokenExpired};
    client_->publish_auth(snapshot_);
}

void NacosAuthenticator::on_attempt_timer(NacosAuthenticator *authenticator) noexcept {
    FIBER_ASSERT(authenticator != nullptr);
    authenticator->start_attempt();
}

void NacosAuthenticator::on_expiry_timer(NacosAuthenticator *authenticator) noexcept {
    FIBER_ASSERT(authenticator != nullptr);
    authenticator->publish_expired();
}

} // namespace fiber::nacos::detail
