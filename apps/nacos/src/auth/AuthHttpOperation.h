#ifndef FIBER_NACOS_AUTH_AUTH_HTTP_OPERATION_H
#define FIBER_NACOS_AUTH_AUTH_HTTP_OPERATION_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <async/Task.h>
#include <common/mem/BufPool.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuth.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <http/ClientHttp1Exchange.h>
#include <http/Http1ClientConnection.h>

namespace fiber::nacos::detail {

struct AuthHttpSuccess {
    std::string access_token;
    std::string username;
    std::int64_t token_ttl = 0;
    bool global_admin = false;
};

class AuthHttpOperation {
public:
    AuthHttpOperation(event::EventLoop &loop, const NacosClientConfig &config, const NacosClientOptions &options,
                      std::size_t server_index, std::string target, std::string_view auth_body);
    ~AuthHttpOperation();

    [[nodiscard]] async::Task<std::expected<AuthHttpSuccess, NacosAuthError>> run() noexcept;
    void cancel() noexcept;

private:
    [[nodiscard]] NacosAuthError make_io_error(common::IoErr error) const noexcept;
    [[nodiscard]] NacosAuthError make_error(NacosAuthErrorCode code) const noexcept;
    [[nodiscard]] bool canceled() const noexcept { return canceled_; }

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosClientOptions *options_ = nullptr;
    std::size_t server_index_ = 0;
    std::string target_;
    std::string host_header_;
    std::string_view auth_body_;
    http::Http1ClientConnection connection_;
    mem::BufPool pool_;
    std::optional<http::ClientHttp1Exchange> exchange_;
    std::string response_body_;
    bool connecting_ = false;
    bool canceled_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_AUTH_AUTH_HTTP_OPERATION_H
