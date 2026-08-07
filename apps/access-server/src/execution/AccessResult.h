#ifndef FIBER_ACCESS_SERVER_ACCESS_RESULT_H
#define FIBER_ACCESS_SERVER_ACCESS_RESULT_H

#include <fiber/common/IoError.h>

#include <cstdint>
#include <expected>
#include <string_view>

namespace fiber::mem {
class BufPool;
}

namespace fiber::access_server {

struct Exception {
    // Both views must reference string literals or storage owned by the request pool.
    std::string_view name;
    std::string_view message;
    std::uint32_t status = 500;

    [[nodiscard]] static constexpr Exception router_not_found() noexcept {
        return {
                .name = "ROUTER_NOT_FOUND",
                .message = "error find router",
                .status = 404,
        };
    }

    [[nodiscard]] static constexpr Exception bad_request() noexcept {
        return {
                .name = "BAD_REQUEST",
                .message = "error find router",
                .status = 400,
        };
    }

    [[nodiscard]] static constexpr Exception entry_error() noexcept {
        return {
                .name = "ENTRY_ERROR",
                .message = "entry error",
                .status = 403,
        };
    }

    [[nodiscard]] static constexpr Exception source_ip_not_allowed() noexcept {
        return {
                .name = "NOT_ALLOW_IP",
                .message = "source ip is not allowed",
                .status = 403,
        };
    }

    [[nodiscard]] static constexpr Exception request_body_too_large() noexcept {
        return {
                .name = "REQ_BODY_TOO_LARGE",
                .message = "request body is too large",
                .status = 413,
        };
    }

    [[nodiscard]] static constexpr Exception unknown(std::string_view message = {}) noexcept {
        return {
                .name = "ACCESS_UNKNOWN_ERROR",
                .message = message.empty() ? std::string_view("unknown error") : message,
                .status = 500,
        };
    }
};

struct Err {
    enum class Kind : std::uint8_t {
        Error, // Raw IO/memory failure without a public Exception payload.
        Exception, // AccessRequestHandler owns the FiberException event.
        UpstreamException, // Proxy attempt already owns the CALL_ERROR event.
    };

    [[nodiscard]] static constexpr Err from_error(common::IoErr error) noexcept {
        return Err{
                .kind = Kind::Error,
                .error = error,
        };
    }

    [[nodiscard]] static constexpr Err from_exception(Exception exception) noexcept {
        return Err{
                .kind = Kind::Exception,
                .exception = exception,
        };
    }

    [[nodiscard]] static constexpr Err from_upstream_exception(Exception exception) noexcept {
        return Err{
                .kind = Kind::UpstreamException,
                .exception = exception,
        };
    }

    Kind kind = Kind::Error;
    common::IoErr error = common::IoErr::Unknown;
    Exception exception{};
};

template<typename T>
using Result = std::expected<T, Err>;

using ExceptionResult = std::expected<Exception, common::IoErr>;

[[nodiscard]] ExceptionResult make_exception(mem::BufPool &pool, std::uint32_t status, std::string_view name,
                                             std::string_view message_prefix, std::string_view detail) noexcept;
[[nodiscard]] ExceptionResult make_url_not_matched_exception(mem::BufPool &pool, std::string_view project) noexcept;
[[nodiscard]] ExceptionResult make_template_script_exception(mem::BufPool &pool, std::string_view detail) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_RESULT_H
