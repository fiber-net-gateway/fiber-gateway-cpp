#ifndef FIBER_NACOS_DTO_BASE_H
#define FIBER_NACOS_DTO_BASE_H

#include <cstdint>
#include <string_view>

#include <fiber/common/json/JsonValue.h>

namespace fiber::nacos::dto {

inline constexpr std::int32_t kResponseSuccess = 200;
inline constexpr std::int32_t kResponseFail = 500;
inline constexpr std::string_view kInternalModule = "internal";
inline constexpr std::string_view kConfigModule = "config";
inline constexpr std::string_view kNamingModule = "naming";

struct RequestBase {
    RequestBase() noexcept { request_id.set_null(); }

    json::Nullable<std::string_view> request_id;
};

struct ConfigRequestBase : RequestBase {
    ConfigRequestBase() noexcept {
        data_id.set_null();
        group.set_null();
        tenant.set_null();
    }

    json::Nullable<std::string_view> data_id;
    json::Nullable<std::string_view> group;
    json::Nullable<std::string_view> tenant;
};

struct NamingRequestBase : RequestBase {
    NamingRequestBase() noexcept {
        namespace_id.set_null();
        service_name.set_null();
        group_name.set_null();
    }

    json::Nullable<std::string_view> namespace_id;
    json::Nullable<std::string_view> service_name;
    json::Nullable<std::string_view> group_name;
};

struct InternalRequestBase : RequestBase {};

struct ResponseBase {
    ResponseBase() noexcept {
        message.set_null();
        request_id.set_null();
    }

    [[nodiscard]] constexpr bool success() const noexcept { return result_code == kResponseSuccess; }

    std::int32_t result_code = kResponseSuccess;
    std::int32_t error_code = 0;
    json::Nullable<std::string_view> message;
    json::Nullable<std::string_view> request_id;
};

} // namespace fiber::nacos::dto

#endif // FIBER_NACOS_DTO_BASE_H
