#include "AccessResult.h"

#include <fiber/common/mem/BufPool.h>

#include <cstring>
#include <limits>

namespace fiber::access_server {

ExceptionResult make_exception(mem::BufPool &pool, std::uint32_t status, std::string_view name,
                               std::string_view message_prefix, std::string_view detail) noexcept {
    if (message_prefix.size() > std::numeric_limits<std::size_t>::max() - detail.size()) {
        return std::unexpected(common::IoErr::NoMem);
    }
    const std::size_t size = message_prefix.size() + detail.size();
    char *message = nullptr;
    if (size != 0) {
        message = pool.alloc<char>(size);
        if (!message) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    if (!message_prefix.empty()) {
        std::memcpy(message, message_prefix.data(), message_prefix.size());
    }
    if (!detail.empty()) {
        std::memcpy(message + message_prefix.size(), detail.data(), detail.size());
    }
    return Exception{
            .name = name,
            .message = size == 0 ? std::string_view{} : std::string_view(message, size),
            .status = status,
    };
}

ExceptionResult make_url_not_matched_exception(mem::BufPool &pool, std::string_view project) noexcept {
    return make_exception(pool, 404, "URL_NOT_MATCHED", "url not matched is project:", project);
}

ExceptionResult make_template_script_exception(mem::BufPool &pool, std::string_view detail) noexcept {
    return make_exception(pool, 500, "TEMPLATE_SCRIPT", "error exec for template expression: ", detail);
}

} // namespace fiber::access_server
