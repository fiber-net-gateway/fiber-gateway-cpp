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

// The error a failed JSON encode into sink is reported as.
fiber::common::IoErr json_encode_error(const fiber::json::StringSink &sink) noexcept {
    return sink.out_of_memory() ? fiber::common::IoErr::NoMem : fiber::common::IoErr::Invalid;
}

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
        // Non-null empty: the pool copy path requires a dereferenceable base.
        // NOLINTNEXTLINE(bugprone-string-constructor)
        return {"", 0};
    }
    auto *data = static_cast<char *>(pool.alloc(value.size(), alignof(char)));
    if (data == nullptr) {
        return {};
    }
    std::memcpy(data, value.data(), value.size());
    return {data, value.size()};
}

// Native string over borrowed bytes (request buffer or exchange pool). Empty inputs are
// normalized to a non-null base for make_native_string.
fiber::script::JsValue native_string_value(std::string_view value) noexcept {
    if (value.empty()) {
        // NOLINTNEXTLINE(bugprone-string-constructor)
        value = std::string_view{"", 0};
    }
    return fiber::script::JsValue::make_native_string(value.data(), value.size());
}

constexpr std::size_t kInitialConstantCacheCapacity = 8;
constexpr std::size_t kConstantCacheMinFreeQuarter = 4; // grow when free slots < capacity/4

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
        fiber::util::decode_cookie_header(field.value_view(),
                                          [&](std::string_view name, std::string_view value) noexcept {
                                              put_string(heap_, cookies_root_, name, value);
                                          });
    }
    return *cookies_root_;
}

const ScriptExchangeCtx::ConstantEntry *ScriptExchangeCtx::cache_find(std::uint64_t hash, ConstType type,
                                                                      std::string_view name) const noexcept {
    if (constant_cache_ == nullptr) {
        return nullptr;
    }
    const std::size_t mask = constant_cache_capacity_ - 1;
    std::size_t slot = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        const ConstantEntry &entry = constant_cache_[slot];
        if (entry.name.data() == nullptr) {
            return nullptr; // empty slot: absent (the table always keeps one empty slot)
        }
        if (entry.hash == hash && entry.type == type && entry.name == name) {
            return &entry;
        }
        slot = (slot + 1) & mask;
    }
}

bool ScriptExchangeCtx::cache_insert(std::uint64_t hash, ConstType type, std::string_view name,
                                     fiber::script::JsValue value) noexcept {
    if (constant_cache_ == nullptr ||
        constant_cache_size_ + 1 > constant_cache_capacity_ - constant_cache_capacity_ / kConstantCacheMinFreeQuarter) {
        const std::size_t new_capacity =
                constant_cache_capacity_ == 0 ? kInitialConstantCacheCapacity : constant_cache_capacity_ * 2;
        auto *fresh = static_cast<ConstantEntry *>(
                exchange_.pool().alloc(new_capacity * sizeof(ConstantEntry), alignof(ConstantEntry)));
        if (fresh == nullptr) {
            return false;
        }
        for (std::size_t i = 0; i < new_capacity; ++i) {
            fresh[i] = ConstantEntry{};
        }
        const std::size_t mask = new_capacity - 1;
        for (std::size_t i = 0; i < constant_cache_capacity_; ++i) {
            const ConstantEntry &old = constant_cache_[i];
            if (old.name.data() == nullptr) {
                continue;
            }
            std::size_t slot = static_cast<std::size_t>(old.hash) & mask;
            while (fresh[slot].name.data() != nullptr) {
                slot = (slot + 1) & mask;
            }
            fresh[slot] = old;
        }
        constant_cache_ = fresh; // the replaced array stays in the request pool (arena)
        constant_cache_capacity_ = new_capacity;
    }

    const std::size_t mask = constant_cache_capacity_ - 1;
    std::size_t slot = static_cast<std::size_t>(hash) & mask;
    while (constant_cache_[slot].name.data() != nullptr) {
        slot = (slot + 1) & mask;
    }
    constant_cache_[slot] = ConstantEntry{.hash = hash, .name = name, .type = type, .value = value};
    ++constant_cache_size_;
    return true;
}

bool ScriptExchangeCtx::resolve_from_request(ConstType type, std::string_view name,
                                             fiber::script::JsValue &out) noexcept {
    switch (type) {
        case ConstType::Header: {
            // Borrowed from the header buffer; first occurrence wins.
            for (const fiber::http::HttpHeaders::HeaderField &field: exchange_.request_headers()) {
                if (const_name_equal(name, field.lowcase_view())) {
                    out = native_string_value(field.value_view());
                    return true;
                }
            }
            break;
        }
        case ConstType::Query: {
            // Decoded values live only for the duration of the sink call, so each match is
            // copied into the exchange pool; the last match wins. Malformed escapes keep
            // the pairs decoded before the error (lenient, as before).
            std::string_view matched{};
            auto decoded = fiber::util::form_decode_query(
                    exchange_.uri().query, [&](std::string_view key, std::string_view value) -> bool {
                        if (!const_name_equal(name, key)) {
                            return true;
                        }
                        matched = copy_to_pool(exchange_.pool(), value);
                        return matched.data() != nullptr; // false aborts the decode as NoMem
                    });
            if (!decoded && decoded.error() == fiber::common::IoErr::NoMem) {
                return false;
            }
            if (matched.data() != nullptr) {
                out = fiber::script::JsValue::make_native_string(matched.data(), matched.size());
                return true;
            }
            break;
        }
        case ConstType::Cookie: {
            // Borrowed from the header buffer; first occurrence wins across all cookie headers.
            fiber::http::HttpHeaders::MatchRange cookie_headers =
                    exchange_.request_headers().get_all("cookie", kCookieHash);
            for (const fiber::http::HttpHeaders::HeaderField &field: cookie_headers) {
                bool found = false;
                fiber::script::JsValue value = fiber::script::JsValue::make_null();
                fiber::util::decode_cookie_header(field.value_view(),
                                                  [&](std::string_view key, std::string_view cookie_value) noexcept {
                                                      if (!found && const_name_equal(name, key)) {
                                                          found = true;
                                                          value = native_string_value(cookie_value);
                                                      }
                                                  });
                if (found) {
                    out = value;
                    return true;
                }
            }
            break;
        }
        case ConstType::Path: {
            // Borrowed from the matcher text; the last pair with a matching name wins,
            // mirroring the previous sequential bind.
            for (const auto &[var_name, var_value]: path_vars_) {
                if (const_name_equal(name, var_name)) {
                    out = native_string_value(var_value);
                }
            }
            return true;
        }
        case ConstType::Count:
            break;
    }
    out = fiber::script::JsValue::make_null();
    return true;
}

fiber::script::AbiResult ScriptExchangeCtx::resolve_constant(ConstType type, std::string_view name,
                                                             std::uint64_t hash) noexcept {
    if (type == ConstType::Count || name.data() == nullptr) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    if (const ConstantEntry *hit = cache_find(hash, type, name); hit != nullptr) {
        return fiber::script::AbiResult::success(hit->value);
    }
    fiber::script::JsValue value{};
    if (!resolve_from_request(type, name, value) || !cache_insert(hash, type, name, value)) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::OutOfMemory);
    }
    return fiber::script::AbiResult::success(value);
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
    fiber::json::StringSink sink(json);
    fiber::json::Generator gen(sink);
    auto encode_result = fiber::script::json::encode_js_value(gen, body);
    if (encode_result != fiber::json::Generator::Result::OK &&
        encode_result != fiber::json::Generator::Result::GenerateComplete) {
        co_return std::unexpected(json_encode_error(sink));
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
    fiber::json::StringSink sink(json);
    fiber::json::Generator gen(sink);
    using R = fiber::json::Generator::Result;
    if (gen.map_open() != R::OK) {
        co_return std::unexpected(json_encode_error(sink));
    }
    if (gen.string("error", 5) != R::OK) {
        co_return std::unexpected(json_encode_error(sink));
    }
    if (gen.string(error_name.data(), error_name.size()) != R::OK) {
        co_return std::unexpected(json_encode_error(sink));
    }
    const R close_result = gen.map_close();
    if (close_result != R::OK && close_result != R::GenerateComplete) {
        co_return std::unexpected(json_encode_error(sink));
    }
    set_response_header("Content-Type", "application/json");
    co_return co_await send_final_with_body(status, json.size(), reinterpret_cast<const std::uint8_t *>(json.data()));
}

} // namespace fiber::http_script
