#ifndef FIBER_ACCESS_SERVER_ACCESS_ERROR_H
#define FIBER_ACCESS_SERVER_ACCESS_ERROR_H

#include <string>
#include <string_view>

namespace fiber::access_server {

struct AccessError {
    int status = 500;
    std::string name = "ACCESS_UNKNOWN_ERROR";
    std::string message = "unknown error";

    [[nodiscard]] static AccessError router_not_found();
    [[nodiscard]] static AccessError bad_request();
    [[nodiscard]] static AccessError url_not_matched(std::string_view project);
    [[nodiscard]] static AccessError entry_error();
    [[nodiscard]] static AccessError source_ip_not_allowed();
    [[nodiscard]] static AccessError request_body_too_large();
    [[nodiscard]] static AccessError template_script(std::string_view detail);
    [[nodiscard]] static AccessError unknown(std::string_view message = {});
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_ERROR_H
