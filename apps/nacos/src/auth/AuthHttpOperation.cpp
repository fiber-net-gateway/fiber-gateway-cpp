#include "AuthHttpOperation.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <common/Assert.h>
#include <common/json/JsonParse.h>
#include <common/json/JsonParser.h>
#include <fiber/nacos/dto/JsonCodec.h>
#include <http/HttpCommon.h>
#include <http/HttpHeaders.h>
#include <net/SocketAddress.h>

namespace fiber::nacos::detail {
namespace {

http::Http1ClientConnectionOptions
make_connection_options(const NacosClientConfig &config, const NacosClientOptions &options, std::size_t server_index) {
    http::Http1ClientConnectionOptions result;
    result.peer_addr = net::SocketAddress(config.server_ips()[server_index], config.http_port());
    result.connect_timeout = options.connect_timeout;
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

AuthHttpOperation::AuthHttpOperation(event::EventLoop &loop, const NacosClientConfig &config,
                                     const NacosClientOptions &options, std::size_t server_index, std::string target,
                                     std::string_view auth_body) :
    loop_(&loop), config_(&config), options_(&options), server_index_(server_index), target_(std::move(target)),
    host_header_(make_host_header(config, server_index)), auth_body_(auth_body),
    connection_(loop, make_connection_options(config, options, server_index)) {}

AuthHttpOperation::~AuthHttpOperation() = default;

NacosAuthError AuthHttpOperation::make_io_error(common::IoErr error) const noexcept {
    return NacosAuthError{
            .code = error == common::IoErr::Canceled ? NacosAuthErrorCode::Canceled : NacosAuthErrorCode::Io,
            .io_error = error,
            .server_index = server_index_,
    };
}

NacosAuthError AuthHttpOperation::make_error(NacosAuthErrorCode code) const noexcept {
    return NacosAuthError{.code = code, .server_index = server_index_};
}

async::Task<std::expected<AuthHttpSuccess, NacosAuthError>> AuthHttpOperation::run() noexcept {
    connecting_ = true;
    auto connect_result = co_await connection_.connect();
    connecting_ = false;
    if (!connect_result) {
        co_return std::unexpected(make_io_error(connect_result.error()));
    }
    if (canceled()) {
        connection_.close();
        co_return std::unexpected(make_io_error(common::IoErr::Canceled));
    }

    exchange_.emplace(connection_, pool_);
    http::HttpHeaders headers(pool_);
    if (!headers.add_view("host", host_header_) ||
        !headers.add_view("content-type", "application/x-www-form-urlencoded") ||
        !headers.add_view("accept", "application/json") || !headers.add_view("connection", "close")) {
        co_return std::unexpected(make_io_error(common::IoErr::NoMem));
    }

    http::Http1RequestHead request;
    request.method = http::HttpMethod::Post;
    request.target = target_;
    request.headers = &headers;
    request.body = http::HttpBodySpec::ContentLength(auth_body_.size());

    auto send_header_result = co_await exchange_->send_header(request, auth_body_.empty(), options_->request_timeout);
    if (!send_header_result) {
        co_return std::unexpected(make_io_error(send_header_result.error()));
    }
    if (!auth_body_.empty()) {
        auto send_body_result =
                co_await exchange_->write_body(reinterpret_cast<const std::uint8_t *>(auth_body_.data()),
                                               auth_body_.size(), true, options_->request_timeout);
        if (!send_body_result) {
            co_return std::unexpected(make_io_error(send_body_result.error()));
        }
    }
    if (canceled()) {
        co_return std::unexpected(make_io_error(common::IoErr::Canceled));
    }

    const http::Http1ResponseHead *response = nullptr;
    for (;;) {
        auto header_result = co_await exchange_->read_header(options_->request_timeout);
        if (!header_result) {
            co_return std::unexpected(make_io_error(header_result.error()));
        }
        response = *header_result;
        if (!response->is_informational()) {
            break;
        }
    }

    if (response->status_code != 200) {
        NacosAuthError error = make_error(NacosAuthErrorCode::HttpStatus);
        error.http_status = response->status_code;
        co_return std::unexpected(error);
    }

    response_body_.clear();
    response_body_.reserve(std::min<std::size_t>(options_->max_auth_response_bytes, 4096));
    for (;;) {
        const std::size_t remaining = options_->max_auth_response_bytes - response_body_.size();
        const std::size_t read_limit = remaining == std::numeric_limits<std::size_t>::max() ? remaining : remaining + 1;
        auto body_result = co_await exchange_->read_body(read_limit, options_->request_timeout);
        if (!body_result) {
            co_return std::unexpected(make_io_error(body_result.error()));
        }
        if (body_result->readable_bytes() > remaining) {
            co_return std::unexpected(make_error(NacosAuthErrorCode::ResponseTooLarge));
        }
        append_chunk(*body_result, response_body_);
        if (body_result->complete()) {
            break;
        }
        if (canceled()) {
            co_return std::unexpected(make_io_error(common::IoErr::Canceled));
        }
    }

    json::JsonParser parser;
    if (!parser.feed(response_body_.data(), response_body_.size())) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::InvalidJson));
    }
    parser.finish();
    dto::resp::AuthTokenResponse token_response;
    if (json::parse_document<parse_auth_token_response>(parser, pool_, token_response) != json::ParseStatus::Done) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::InvalidJson));
    }
    if (!token_response.access_token.is_present() || token_response.access_token.value().empty()) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::MissingAccessToken));
    }
    if (token_response.token_ttl <= 0) {
        co_return std::unexpected(make_error(NacosAuthErrorCode::InvalidTokenTtl));
    }

    AuthHttpSuccess success;
    success.access_token.assign(token_response.access_token.value());
    if (token_response.username.is_present()) {
        success.username.assign(token_response.username.value());
    } else {
        success.username = config_->username();
    }
    success.token_ttl = token_response.token_ttl;
    success.global_admin = token_response.global_admin;

    exchange_.reset();
    connection_.close();
    co_return success;
}

void AuthHttpOperation::cancel() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (canceled_) {
        return;
    }
    canceled_ = true;
    if (connecting_) {
        return;
    }
    if (exchange_ && exchange_->valid()) {
        (void) exchange_->abort(common::IoErr::Canceled);
        return;
    }
    if (connection_.connected() || connection_.valid()) {
        connection_.close();
    }
}

} // namespace fiber::nacos::detail
