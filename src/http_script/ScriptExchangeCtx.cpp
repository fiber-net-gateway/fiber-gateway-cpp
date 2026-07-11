#include "ScriptExchangeCtx.h"

#include "../common/json/JsonEncode.h"
#include "../common/util/CookieCodec.h"
#include "../common/util/UrlForm.h"
#include "../http/HttpBodySpec.h"
#include "../http/HttpExchangeIo.h"
#include "../script/JsValue.h"
#include "../script/gc/GcInternal.h"
#include "../script/json/JsValueEncode.h"

#include <string>
#include <string_view>

namespace fiber::http_script {

namespace {

// Writes generated JSON into a std::string buffer. Used by write_json to materialize a
// JsValue as bytes before handing them to HttpExchange::write_body.
class StringSink final : public fiber::json::OutputSink {
public:
    explicit StringSink(std::string &out) noexcept : out_(out) {}
    [[nodiscard]] bool write(const char *data, std::size_t len) noexcept override {
        out_.append(data, len);
        return true;
    }

private:
    std::string &out_;
};

// Stores a string value into the object at obj_root under key, overwriting any prior
// value (mirrors Java ObjectNode.put). Returns false on allocation failure. The new
// string is held in a stack-scoped root across the allocation via LocalMark.
bool put_string(fiber::script::GcHeap &heap, fiber::script::ValueHandle obj_root, std::string_view key,
                std::string_view value) noexcept {
    fiber::script::GcHeap::LocalMark mark(heap);
    fiber::script::ValueHandle item = heap.local_value();
    if (!item) {
        return false;
    }
    *item = fiber::script::JsValue::make_string(heap, value.data(), value.size());
    if (fiber::script::js_value_type(*item) != fiber::script::JsNodeType::String) {
        return false;
    }
    return fiber::script::gc_object_set_key(&heap, obj_root, key.data(), key.size(), *item);
}

// Builds a String JsValue; aborts on allocation failure. Used by the route-variable
// accessors to materialize a borrowed name/value view into the GC heap.
fiber::script::ScriptResult make_string_value(fiber::script::GcHeap &heap, std::string_view text) noexcept {
    fiber::script::JsValue v = fiber::script::JsValue::make_string(heap, text.data(), text.size());
    if (fiber::script::js_value_type(v) != fiber::script::JsNodeType::String) {
        return fiber::script::ScriptResult::abort(fiber::script::ScriptAbortReason::OutOfMemory);
    }
    return fiber::script::ScriptResult::success(v);
}

// Compares a header/cookie name against a pre-normalized key (lowercase, '-' folded to '_').
// `name` is matched under the same normalization so that $header.x_forwarded_for matches the
// "X-Forwarded-For" header.
bool name_matches_normalized(std::string_view name, std::string_view norm_key) noexcept {
    if (name.size() != norm_key.size()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c == '-') {
            c = '_';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        if (c != static_cast<unsigned char>(norm_key[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

ScriptExchangeCtx::ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap) noexcept :
    exchange_(exchange), heap_(heap), pending_headers_(exchange.pool()) {}

fiber::script::JsValue ScriptExchangeCtx::query() noexcept {
    if (query_root_ && fiber::script::js_value_type(*query_root_) == fiber::script::JsNodeType::Object) {
        return *query_root_;
    }
    if (!query_root_) {
        query_root_ = heap_.global_value();
        if (!query_root_) {
            return fiber::script::JsValue::make_undefined();
        }
    }
    *query_root_ = fiber::script::JsValue::make_object(heap_, 0);
    if (fiber::script::js_value_type(*query_root_) != fiber::script::JsNodeType::Object) {
        return fiber::script::JsValue::make_undefined(); // OOM; slot stays undefined, retries next call
    }
    std::string_view q = exchange_.uri().query;
    auto query_io = fiber::util::form_decode_query(
            q, [&](std::string_view k, std::string_view v) -> bool { return put_string(heap_, query_root_, k, v); });
    (void) query_io; // malformed percent escapes leave the partial object (lenient)
    return *query_root_;
}

fiber::script::JsValue ScriptExchangeCtx::headers() noexcept {
    if (headers_root_ && fiber::script::js_value_type(*headers_root_) == fiber::script::JsNodeType::Object) {
        return *headers_root_;
    }
    if (!headers_root_) {
        headers_root_ = heap_.global_value();
        if (!headers_root_) {
            return fiber::script::JsValue::make_undefined();
        }
    }
    *headers_root_ = fiber::script::JsValue::make_object(heap_, 0);
    if (fiber::script::js_value_type(*headers_root_) != fiber::script::JsNodeType::Object) {
        return fiber::script::JsValue::make_undefined();
    }
    for (const fiber::http::HttpHeaders::HeaderField &field: exchange_.request_headers()) {
        put_string(heap_, headers_root_, field.name_view(), field.value_view());
    }
    return *headers_root_;
}

fiber::script::JsValue ScriptExchangeCtx::cookies() noexcept {
    if (cookies_root_ && fiber::script::js_value_type(*cookies_root_) == fiber::script::JsNodeType::Object) {
        return *cookies_root_;
    }
    if (!cookies_root_) {
        cookies_root_ = heap_.global_value();
        if (!cookies_root_) {
            return fiber::script::JsValue::make_undefined();
        }
    }
    *cookies_root_ = fiber::script::JsValue::make_object(heap_, 0);
    if (fiber::script::js_value_type(*cookies_root_) != fiber::script::JsNodeType::Object) {
        return fiber::script::JsValue::make_undefined();
    }
    fiber::http::HttpHeaders::MatchRange cookie_headers = exchange_.request_headers().get_all("cookie");
    for (const fiber::http::HttpHeaders::HeaderField &field: cookie_headers) {
        fiber::util::decode_cookie_header(field.value_view(), [&](std::string_view name, std::string_view value) {
            put_string(heap_, cookies_root_, name, value);
        });
    }
    return *cookies_root_;
}

fiber::script::ScriptResult ScriptExchangeCtx::lookup_property(fiber::script::GcHeap &heap,
                                                               fiber::script::JsValue object,
                                                               std::string_view key) noexcept {
    using namespace fiber::script;
    if (js_value_type(object) != JsNodeType::Object) {
        return ScriptResult::success(JsValue::make_undefined());
    }
    GcHeap::LocalMark mark(heap);
    ValueHandle found = heap.local_value();
    if (!found) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    *found = JsValue::make_undefined();
    if (!gc_object_get_key(&heap, ConstValueHandle(&object), key.data(), key.size(), found)) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (js_value_type(*found) == JsNodeType::Undefined) {
        return ScriptResult::success(JsValue::make_undefined()); // absent
    }
    return ScriptResult::success(*found);
}

fiber::script::ScriptResult ScriptExchangeCtx::path_var(fiber::script::GcHeap &heap,
                                                        std::string_view name) const noexcept {
    for (const auto &pv: path_vars_) {
        if (pv.first == name) {
            return make_string_value(heap, pv.second);
        }
    }
    // Absent -> null (mirrors Java NullNode; undefined is not JSON-encodable here).
    return fiber::script::ScriptResult::success(fiber::script::JsValue::make_null());
}

fiber::script::ScriptResult ScriptExchangeCtx::query_var(fiber::script::GcHeap &heap, std::string_view name) noexcept {
    fiber::script::ScriptResult result = lookup_property(heap, query(), name);
    if (result.is_success() && fiber::script::js_value_type(result.value()) == fiber::script::JsNodeType::Undefined) {
        return fiber::script::ScriptResult::success(fiber::script::JsValue::make_null());
    }
    return result;
}

fiber::script::ScriptResult ScriptExchangeCtx::header_var(fiber::script::GcHeap &heap,
                                                          std::string_view norm_key) const noexcept {
    for (const fiber::http::HttpHeaders::HeaderField &field: exchange_.request_headers()) {
        if (name_matches_normalized(field.lowcase_view(), norm_key)) {
            return make_string_value(heap, field.value_view());
        }
    }
    return fiber::script::ScriptResult::success(fiber::script::JsValue::make_null());
}

fiber::script::ScriptResult ScriptExchangeCtx::cookie_var(fiber::script::GcHeap &heap,
                                                          std::string_view norm_key) const noexcept {
    fiber::http::HttpHeaders::MatchRange cookie_headers = exchange_.request_headers().get_all("cookie");
    for (const fiber::http::HttpHeaders::HeaderField &field: cookie_headers) {
        std::string_view matched;
        bool found = false;
        fiber::util::decode_cookie_header(field.value_view(), [&](std::string_view name, std::string_view value) {
            if (!found && name_matches_normalized(name, norm_key)) {
                found = true;
                matched = value;
            }
        });
        if (found) {
            return make_string_value(heap, matched);
        }
    }
    return fiber::script::ScriptResult::success(fiber::script::JsValue::make_null());
}

fiber::script::ScriptResult ScriptExchangeCtx::req_field(fiber::script::GcHeap &heap,
                                                         std::string_view field) const noexcept {
    if (field == "uri") {
        return make_string_value(heap, exchange_.uri().unparsed_uri);
    }
    if (field == "method") {
        return make_string_value(heap, exchange_.method_view());
    }
    if (field == "path") {
        return make_string_value(heap, exchange_.uri().path);
    }
    if (field == "query") {
        return make_string_value(heap, exchange_.uri().query);
    }
    return fiber::script::ScriptResult::success(fiber::script::JsValue::make_undefined());
}

void ScriptExchangeCtx::set_response_header(std::string_view name, std::string_view value) noexcept {
    if (header_sent_) {
        return;
    }
    pending_headers_.set(name, value);
}

void ScriptExchangeCtx::add_response_header(std::string_view name, std::string_view value) noexcept {
    if (header_sent_) {
        return;
    }
    pending_headers_.add(name, value);
}

fiber::async::Task<fiber::common::IoResult<void>>
ScriptExchangeCtx::send_final_with_body(int status, std::size_t content_length, const std::uint8_t *data) noexcept {
    fiber::http::OutgoingHeaderBlockView header{};
    header.kind = fiber::http::OutgoingHeaderKind::Final;
    header.status_code = status;
    header.headers = &pending_headers_;
    header.body = fiber::http::HttpBodySpec::ContentLength(content_length);
    header.connection_mode = fiber::http::ResponseConnectionMode::Auto;
    header.end_stream = false;

    auto header_result = co_await exchange_.send_header(header);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    header_sent_ = true;

    auto body_result = co_await exchange_.write_body(data, content_length, true);
    if (!body_result) {
        co_return std::unexpected(body_result.error());
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::Task<fiber::common::IoResult<void>>
ScriptExchangeCtx::write_json(int status, const fiber::script::JsValue &body) noexcept {
    std::string json;
    StringSink sink(json);
    fiber::json::Generator gen(sink);
    auto encode_result = fiber::script::json::encode_js_value(gen, body);
    if (encode_result != fiber::json::Generator::Result::OK &&
        encode_result != fiber::json::Generator::Result::GenerateComplete) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    set_response_header("Content-Type", "application/json");
    co_return co_await send_final_with_body(status, json.size(), reinterpret_cast<const std::uint8_t *>(json.data()));
}

fiber::async::Task<fiber::common::IoResult<void>>
ScriptExchangeCtx::write_raw_bytes(int status, const std::uint8_t *data, std::size_t len) noexcept {
    co_return co_await send_final_with_body(status, len, data);
}

fiber::async::Task<fiber::common::IoResult<void>> ScriptExchangeCtx::write_empty(int status) noexcept {
    co_return co_await send_final_with_body(status, 0, nullptr);
}

} // namespace fiber::http_script
