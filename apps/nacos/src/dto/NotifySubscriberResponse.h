#ifndef FIBER_NACOS_DTO_NOTIFY_SUBSCRIBER_RESPONSE_H
#define FIBER_NACOS_DTO_NOTIFY_SUBSCRIBER_RESPONSE_H

#include <string_view>

#include "Base.h"

namespace fiber::nacos::dto::resp {

struct NotifySubscriberResponse : ResponseBase {
    static constexpr std::string_view kTypeName = "NotifySubscriberResponse";
};

} // namespace fiber::nacos::dto::resp

#endif // FIBER_NACOS_DTO_NOTIFY_SUBSCRIBER_RESPONSE_H
