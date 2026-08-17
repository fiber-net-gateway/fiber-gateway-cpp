#include <fiber/http_script/ScriptExchangeCtx.h>

#include <arpa/inet.h>

#include <fiber/common/Assert.h>
#include <fiber/common/json/JsonEncode.h>
#include <fiber/common/util/CookieCodec.h>
#include <fiber/common/util/UrlForm.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchangeIo.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/json/JsValueEncode.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace fiber::http_script {

namespace {

// Pre-hashed Cookie header name for per-request cookie parsing.
constexpr std::uint64_t kCookieHash = fiber::http::http_header_name_hash("cookie");

// Writes generated JSON into a std::string buffer. Used by write_json to materialize a
// JsValue as bytes before handing them to HttpExchange::write_all.
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

std::string_view copy_to_pool(fiber::mem::BufPool &pool, std::string_view value) noexcept {
    if (value.empty()) {
        return std::string_view("", 0);
    }
    auto *data = static_cast<char *>(pool.alloc(value.size(), alignof(char)));
    if (data == nullptr) {
        return {};
    }
    std::memcpy(data, value.data(), value.size());
    return {data, value.size()};
}

} // namespace

ScriptExchangeCtx::ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap) noexcept :
    ScriptExchangeCtx(exchange, heap,
                      ScriptConnectionInfo{
                              .scheme = exchange.scheme(),
                              .tls = exchange.scheme() == "https",
                      }) {}

ScriptExchangeCtx::ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap,
                                     ScriptConnectionInfo connection) noexcept :
    ScriptExchangeCtx(exchange, heap, connection, make_script_request_body(exchange)) {}

ScriptExchangeCtx::ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap,
                                     ScriptConnectionInfo connection, ScriptRequestBody request_body) noexcept :
    ScriptExchangeCtx(exchange, heap, connection, request_body, fiber::http::make_http_response_writer(exchange)) {}

ScriptExchangeCtx::ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap,
                                     ScriptConnectionInfo connection, ScriptRequestBody request_body,
                                     fiber::http::HttpResponseWriter response_writer) noexcept :
    exchange_(exchange), response_writer_(response_writer), heap_(heap), connection_(connection),
    request_body_(request_body), pending_headers_(exchange.pool()) {
    FIBER_ASSERT(request_body_.valid());
    FIBER_ASSERT(response_writer_.valid());
}

fiber::script::AbiResult ScriptExchangeCtx::remote_address_constant() noexcept {
    if (!fiber::script::js_value_is_undefined(remote_addr_constant_)) {
        return fiber::script::AbiResult::success(remote_addr_constant_);
    }

    std::array<char, INET6_ADDRSTRLEN> buffer{};
    const auto &ip = exchange_.remote_addr().ip();
    const void *source = nullptr;
    int family = AF_INET;
    std::array<std::uint8_t, fiber::net::IpAddress::kV4Size> v4{};
    if (ip.is_v4()) {
        v4 = ip.v4_bytes();
        source = v4.data();
    } else {
        family = AF_INET6;
        source = ip.v6_bytes().data();
    }
    const char *result = ::inet_ntop(family, source, buffer.data(), static_cast<socklen_t>(buffer.size()));
    if (result == nullptr) {
        remote_addr_constant_ = fiber::script::JsValue::make_null();
        return fiber::script::AbiResult::success(remote_addr_constant_);
    }

    std::string_view value = copy_to_pool(exchange_.pool(), result);
    if (value.data() == nullptr) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::OutOfMemory);
    }
    remote_addr_constant_ = fiber::script::JsValue::make_native_string(value.data(), value.size());
    return fiber::script::AbiResult::success(remote_addr_constant_);
}

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
    fiber::http::HttpHeaders::MatchRange cookie_headers = exchange_.request_headers().get_all("cookie", kCookieHash);
    for (const fiber::http::HttpHeaders::HeaderField &field: cookie_headers) {
        fiber::util::decode_cookie_header(field.value_view(), [&](std::string_view name, std::string_view value) {
            put_string(heap_, cookies_root_, name, value);
        });
    }
    return *cookies_root_;
}

fiber::common::IoResult<void>
ScriptExchangeCtx::prepare_constants(const ConstPackage &package,
                                     std::span<const IndexedConstValue> external_values) noexcept {
    if (const_package_identity_ != nullptr) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }

    constant_slot_count_ = package.size();
    const_package_identity_ = package.identity();
    if (constant_slot_count_ != 0) {
        constant_slots_ = static_cast<fiber::script::JsValue *>(exchange_.pool().alloc(
                constant_slot_count_ * sizeof(fiber::script::JsValue), alignof(fiber::script::JsValue)));
        if (constant_slots_ == nullptr) {
            const_package_identity_ = nullptr;
            constant_slot_count_ = 0;
            return std::unexpected(fiber::common::IoErr::NoMem);
        }
        for (std::size_t i = 0; i < constant_slot_count_; ++i) {
            constant_slots_[i] = fiber::script::JsValue::make_null();
        }
    }

    auto set_string = [&](ConstIndex index, std::string_view value, bool copy) -> bool {
        if (index >= constant_slot_count_) {
            return false;
        }
        if (copy) {
            value = copy_to_pool(exchange_.pool(), value);
            if (value.data() == nullptr) {
                return false;
            }
        }
        constant_slots_[index] = fiber::script::JsValue::make_native_string(value.data(), value.size());
        return true;
    };

    if (!package.entries(ConstType::Header).empty()) {
        for (const fiber::http::HttpHeaders::HeaderField &field: exchange_.request_headers()) {
            const ConstIndex index = package.find(ConstType::Header, field.lowcase_view());
            if (index == kInvalidConstIndex) {
                continue;
            }
            if (index >= constant_slot_count_) {
                return std::unexpected(fiber::common::IoErr::Invalid);
            }
            if (fiber::script::js_value_type(constant_slots_[index]) == fiber::script::JsNodeType::Null &&
                !set_string(index, field.value_view(), false)) {
                return std::unexpected(fiber::common::IoErr::NoMem);
            }
        }
    }

    if (!package.entries(ConstType::Query).empty()) {
        auto decoded = fiber::util::form_decode_query(
                exchange_.uri().query, [&](std::string_view name, std::string_view value) -> bool {
                    const ConstIndex index = package.find(ConstType::Query, name);
                    return index == kInvalidConstIndex || set_string(index, value, true);
                });
        if (!decoded && decoded.error() == fiber::common::IoErr::NoMem) {
            return std::unexpected(fiber::common::IoErr::NoMem);
        }
        // Preserve the existing lenient behavior: malformed escapes leave any values
        // decoded before the error available to the script.
    }

    if (!package.entries(ConstType::Cookie).empty()) {
        fiber::http::HttpHeaders::MatchRange cookie_headers =
                exchange_.request_headers().get_all("cookie", kCookieHash);
        for (const fiber::http::HttpHeaders::HeaderField &field: cookie_headers) {
            fiber::util::decode_cookie_header(field.value_view(), [&](std::string_view name, std::string_view value) {
                const ConstIndex index = package.find(ConstType::Cookie, name);
                if (index != kInvalidConstIndex && index < constant_slot_count_ &&
                    fiber::script::js_value_type(constant_slots_[index]) == fiber::script::JsNodeType::Null) {
                    (void) set_string(index, value, false);
                }
            });
        }
    }

    if (!bind_constants(external_values)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return {};
}

bool ScriptExchangeCtx::bind_constants(std::span<const IndexedConstValue> values) noexcept {
    if (const_package_identity_ == nullptr) {
        return false;
    }
    for (const IndexedConstValue &entry: values) {
        if (entry.index == kInvalidConstIndex) {
            continue;
        }
        if (!bind_constant(entry.index, entry.value)) {
            return false;
        }
    }
    return true;
}

bool ScriptExchangeCtx::bind_constant(ConstIndex index, std::string_view value) noexcept {
    if (const_package_identity_ == nullptr || index >= constant_slot_count_) {
        return false;
    }
    if (value.empty()) {
        value = std::string_view("", 0);
    }
    constant_slots_[index] = fiber::script::JsValue::make_native_string(value.data(), value.size());
    return true;
}

bool ScriptExchangeCtx::bind_path_constants(
        const ConstPackage &package,
        std::span<const std::pair<std::string_view, std::string_view>> path_values) noexcept {
    if (const_package_identity_ != package.identity()) {
        return false;
    }
    for (const auto &[name, value]: path_values) {
        const ConstIndex index = package.find(ConstType::Path, name);
        if (index != kInvalidConstIndex && !bind_constant(index, value)) {
            return false;
        }
    }
    return true;
}

void ScriptExchangeCtx::clear_constants(std::span<const ConstIndex> indices) noexcept {
    for (ConstIndex index: indices) {
        if (index != kInvalidConstIndex && index < constant_slot_count_) {
            constant_slots_[index] = fiber::script::JsValue::make_null();
        }
    }
}

fiber::script::AbiResult ScriptExchangeCtx::constant(const void *package_identity, ConstIndex index) const noexcept {
    if (package_identity == nullptr || package_identity != const_package_identity_ || index >= constant_slot_count_ ||
        constant_slots_ == nullptr) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    return fiber::script::AbiResult::success(constant_slots_[index]);
}

fiber::script::AbiResult ScriptExchangeCtx::lookup_property(fiber::script::GcHeap &heap, fiber::script::JsValue object,
                                                            std::string_view key) noexcept {
    using namespace fiber::script;
    if (js_value_type(object) != JsNodeType::Object) {
        return AbiResult::success(JsValue::make_undefined());
    }
    GcHeap::LocalMark mark(heap);
    ValueHandle found = heap.local_value();
    if (!found) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    *found = JsValue::make_undefined();
    if (!gc_object_get_key(&heap, ConstValueHandle(&object), key.data(), key.size(), found)) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (js_value_type(*found) == JsNodeType::Undefined) {
        return AbiResult::success(JsValue::make_undefined()); // absent
    }
    return AbiResult::success(*found);
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

    auto header_result = co_await response_writer_.send_header(header);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    header_sent_ = true;

    auto body_result = co_await response_writer_.write_all(data, content_length, true);
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

fiber::async::Task<fiber::common::IoResult<void>>
ScriptExchangeCtx::write_error_json(int status, std::string_view error_name) noexcept {
    std::string json;
    StringSink sink(json);
    fiber::json::Generator gen(sink);
    using R = fiber::json::Generator::Result;
    if (gen.map_open() != R::OK) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (gen.string("error", 5) != R::OK) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (gen.string(error_name.data(), error_name.size()) != R::OK) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    const R close_result = gen.map_close();
    if (close_result != R::OK && close_result != R::GenerateComplete) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    set_response_header("Content-Type", "application/json");
    co_return co_await send_final_with_body(status, json.size(), reinterpret_cast<const std::uint8_t *>(json.data()));
}

} // namespace fiber::http_script
