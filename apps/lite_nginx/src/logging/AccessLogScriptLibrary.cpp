#include "AccessLogScriptLibrary.h"

#include <algorithm>
#include <limits>

#include "script/JsValue.h"
#include "script/ScriptResult.h"
#include "script/std/StdLibrary.h"

namespace fiber::lite_nginx::logging {
namespace {

using fiber::script::AbiResult;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ScriptAbortReason;

AbiResult make_string(fiber::script::GcHeap &heap, std::string_view value) noexcept {
    JsValue result = JsValue::make_string(heap, value.data(), value.size());
    if (fiber::script::js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

AbiResult make_optional_string(fiber::script::GcHeap &heap, std::string_view value) noexcept {
    return value.empty() ? AbiResult::success(JsValue::make_null()) : make_string(heap, value);
}

JsValue make_uint(std::uint64_t value) noexcept {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return JsValue::make_integer(static_cast<std::int64_t>(std::min(value, kMax)));
}

} // namespace

AccessLogScriptLibrary::AccessLogScriptLibrary(fiber::script::std_lib::StdLibrary &shared,
                                               const std::vector<std::string> &path_var_names) :
    RouteScriptLibrary(shared, path_var_names) {
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        fields_[i].field = static_cast<Field>(i);
        callables_[i].kind = HostCallable::Kind::SyncConstant;
        callables_[i].userdata = &fields_[i];
        callables_[i].constant = &AccessLogScriptLibrary::field_fn;
        callables_[i].debug_name = "$access_log";
    }
}

const fiber::script::Library::HostCallable *AccessLogScriptLibrary::resolve_constant(std::string_view namespace_name,
                                                                                     std::string_view key) const {
    Field field;
    if (namespace_name == "$access") {
        if (key == "request_id") {
            field = Field::RequestId;
        } else if (key == "server") {
            field = Field::Server;
        } else if (key == "location") {
            field = Field::Location;
        } else if (key == "status") {
            field = Field::Status;
        } else if (key == "body_bytes_sent") {
            field = Field::BodyBytesSent;
        } else if (key == "request_time_us") {
            field = Field::RequestTimeUs;
        } else if (key == "outcome") {
            field = Field::Outcome;
        } else {
            return nullptr;
        }
    } else if (namespace_name == "$upstream") {
        if (key == "host") {
            field = Field::UpstreamHost;
        } else if (key == "port") {
            field = Field::UpstreamPort;
        } else if (key == "status") {
            field = Field::UpstreamStatus;
        } else if (key == "time_us") {
            field = Field::UpstreamTimeUs;
        } else if (key == "error") {
            field = Field::UpstreamError;
        } else {
            return nullptr;
        }
    } else {
        return RouteScriptLibrary::resolve_constant(namespace_name, key);
    }
    return &callables_[static_cast<std::size_t>(field)];
}

AbiResult AccessLogScriptLibrary::field_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *field = static_cast<const FieldRef *>(userdata);
    auto *base = static_cast<fiber::http_script::ScriptExchangeCtx *>(frame.attach);
    auto *ctx = static_cast<AccessLogEvalContext *>(base);
    if (field == nullptr || ctx == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    const AccessLogScriptData &data = ctx->data();
    switch (field->field) {
        case Field::RequestId:
            return AbiResult::success(make_uint(data.request_id));
        case Field::Server:
            return make_optional_string(frame.runtime, data.server_name);
        case Field::Location:
            return make_optional_string(frame.runtime, data.location_pattern);
        case Field::Status:
            return AbiResult::success(JsValue::make_integer(data.status));
        case Field::BodyBytesSent:
            return AbiResult::success(make_uint(data.body_bytes_sent));
        case Field::RequestTimeUs:
            return AbiResult::success(make_uint(data.request_time_us));
        case Field::Outcome:
            return make_string(frame.runtime, data.outcome);
        case Field::UpstreamHost:
            return make_optional_string(frame.runtime, data.upstream_host);
        case Field::UpstreamPort:
            return data.upstream_started ? AbiResult::success(JsValue::make_integer(data.upstream_port))
                                         : AbiResult::success(JsValue::make_null());
        case Field::UpstreamStatus:
            return data.upstream_status > 0 ? AbiResult::success(JsValue::make_integer(data.upstream_status))
                                            : AbiResult::success(JsValue::make_null());
        case Field::UpstreamTimeUs:
            return data.upstream_started ? AbiResult::success(make_uint(data.upstream_time_us))
                                         : AbiResult::success(JsValue::make_null());
        case Field::UpstreamError:
            return data.upstream_error == fiber::common::IoErr::None
                           ? AbiResult::success(JsValue::make_null())
                           : make_string(frame.runtime, fiber::common::io_err_name(data.upstream_error));
        case Field::Count:
            break;
    }
    return AbiResult::abort(ScriptAbortReason::InvalidState);
}

} // namespace fiber::lite_nginx::logging
