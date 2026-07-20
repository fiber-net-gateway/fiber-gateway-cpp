#ifndef FIBER_NACOS_DTO_CONFIG_QUERY_REQUEST_H
#define FIBER_NACOS_DTO_CONFIG_QUERY_REQUEST_H

#include <string_view>

#include "Base.h"

namespace fiber::nacos::dto::req {

struct ConfigQueryRequest : ConfigRequestBase {
    static constexpr std::string_view kTypeName = "ConfigQueryRequest";
    static constexpr std::string_view kModule = kConfigModule;

    ConfigQueryRequest() noexcept { tag.set_null(); }

    [[nodiscard]] static ConfigQueryRequest build(std::string_view data_id, std::string_view group,
                                                  std::string_view tenant) noexcept {
        ConfigQueryRequest request;
        request.data_id.set_present(data_id);
        request.group.set_present(group);
        request.tenant.set_present(tenant);
        return request;
    }

    json::Nullable<std::string_view> tag;
    bool notify = false;
};

} // namespace fiber::nacos::dto::req

#endif // FIBER_NACOS_DTO_CONFIG_QUERY_REQUEST_H
