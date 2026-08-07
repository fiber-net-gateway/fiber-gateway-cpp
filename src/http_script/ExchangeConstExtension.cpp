#include <fiber/http_script/ExchangeConstExtension.h>

#include <fiber/http_script/ScriptExchangeCtx.h>

#include <fiber/script/JsValue.h>

#include <array>

namespace fiber::http_script {
namespace {

fiber::script::AbiResult native_string(std::string_view value) noexcept {
    if (value.empty()) {
        value = std::string_view("", 0);
    }
    return fiber::script::AbiResult::success(fiber::script::JsValue::make_native_string(value.data(), value.size()));
}

std::string_view http_version_text(fiber::http::HttpVersion version) noexcept {
    switch (version) {
        case fiber::http::HttpVersion::HTTP_0_9:
            return "HTTP/0.9";
        case fiber::http::HttpVersion::HTTP_1_0:
            return "HTTP/1.0";
        case fiber::http::HttpVersion::HTTP_1_1:
            return "HTTP/1.1";
        case fiber::http::HttpVersion::HTTP_2_0:
            return "HTTP/2";
        case fiber::http::HttpVersion::HTTP_3_0:
            return "HTTP/3";
    }
    return {};
}

} // namespace

struct ExchangeConstExtension::Table {
    std::array<FieldRef, static_cast<std::size_t>(Field::Count)> fields{};
    std::array<HostCallable, static_cast<std::size_t>(Field::Count)> callables{};

    Table() noexcept {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            fields[i].field = static_cast<Field>(i);
            callables[i].kind = HostCallable::Kind::SyncConstant;
            callables[i].userdata = &fields[i];
            callables[i].constant = &ExchangeConstExtension::constant_fn;
            callables[i].debug_name = i < static_cast<std::size_t>(Field::ConnRemoteAddr) ? "$req" : "$conn";
        }
    }
};

const ExchangeConstExtension::Table &ExchangeConstExtension::table() noexcept {
    static const Table kTable;
    return kTable;
}

const fiber::script::std_lib::StdLibrary::ExtOps &ExchangeConstExtension::ops() noexcept {
    static const fiber::script::std_lib::StdLibrary::ExtOps kOps{
            .resolve_constant = &ExchangeConstExtension::resolve_constant_op,
    };
    return kOps;
}

const ExchangeConstExtension::HostCallable *
ExchangeConstExtension::resolve_constant_op(void *ctx, std::string_view namespace_name, std::string_view key) noexcept {
    return ctx == nullptr ? nullptr : resolve_constant(namespace_name, key);
}

const ExchangeConstExtension::HostCallable *ExchangeConstExtension::resolve_constant(std::string_view namespace_name,
                                                                                     std::string_view key) noexcept {
    Field field;
    if (namespace_name == "$req") {
        if (key == "uri") {
            field = Field::ReqUri;
        } else if (key == "method") {
            field = Field::ReqMethod;
        } else if (key == "path") {
            field = Field::ReqPath;
        } else if (key == "query") {
            field = Field::ReqQuery;
        } else {
            return nullptr;
        }
    } else if (namespace_name == "$conn") {
        if (key == "remote_addr") {
            field = Field::ConnRemoteAddr;
        } else if (key == "remote_port") {
            field = Field::ConnRemotePort;
        } else if (key == "http_version") {
            field = Field::ConnHttpVersion;
        } else if (key == "scheme") {
            field = Field::ConnScheme;
        } else if (key == "tls") {
            field = Field::ConnTls;
        } else {
            return nullptr;
        }
    } else {
        return nullptr;
    }
    return &table().callables[static_cast<std::size_t>(field)];
}

fiber::script::AbiResult ExchangeConstExtension::constant_fn(void *userdata, const HostCallFrame &frame) noexcept {
    const auto *ref = static_cast<const FieldRef *>(userdata);
    auto *context = static_cast<ScriptExchangeCtx *>(frame.attach);
    if (ref == nullptr || context == nullptr) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }

    switch (ref->field) {
        case Field::ReqUri:
            return native_string(context->exchange_.uri().unparsed_uri);
        case Field::ReqMethod:
            return native_string(context->exchange_.method_view());
        case Field::ReqPath:
            return native_string(context->exchange_.uri().path);
        case Field::ReqQuery:
            return native_string(context->exchange_.uri().query);
        case Field::ConnRemoteAddr:
            return context->remote_address_constant();
        case Field::ConnRemotePort:
            return fiber::script::AbiResult::success(
                    fiber::script::JsValue::make_integer(context->exchange_.remote_addr().port()));
        case Field::ConnHttpVersion:
            return native_string(http_version_text(context->exchange_.version()));
        case Field::ConnScheme:
            return native_string(context->connection_.scheme);
        case Field::ConnTls:
            return fiber::script::AbiResult::success(fiber::script::JsValue::make_boolean(context->connection_.tls));
        case Field::Count:
            break;
    }
    return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
}

} // namespace fiber::http_script
