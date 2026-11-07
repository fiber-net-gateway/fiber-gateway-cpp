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

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>

namespace fiber::http_script {

namespace {

// Pre-hashed Cookie header name for per-request cookie parsing.
constexpr std::uint64_t kCookieHash = fiber::http::http_header_name_hash("cookie");

// Sink node payload capacity matching the io-buf node pool's fixed node size (same
// formula as HttpBodyPipe's coalesce buffers), so every sink allocation is a freelist pop.
constexpr std::size_t kJsonNodeCapacity = static_cast<std::size_t>(16 * 1024) - fiber::mem::kIoBufControlBlockSize;

// A JSON body goes out each time this much is encoded, so a large response never exists whole.
constexpr std::size_t kJsonFlushBytes = static_cast<std::size_t>(64 * 1024);

// Writes generated JSON directly into an IoBufChain — the transport buffer write_all
// consumes — with no staging copy. Bytes land in the tail node at once but join the chain
// only at commit(), so the many tiny writes of a generator cost a compare and a copy. Call
// commit() before using the chain; moving the chain away after that is fine, the next write
// starts a fresh node. The only failure mode is pool allocation failure, reported as
// write() == false (the OutputSink OOM channel); the Generator surfaces that as ErrorState,
// indistinguishable from value-shape encode errors at the Result level, so the sink records
// it in oom_ for the caller's NoMem/Invalid mapping. Every node is allocated and solely
// owned here (no second handles), so the free region is safe to fill without the unique()
// guard the pipe paths need.
class ChainSink final : public fiber::json::OutputSink {
public:
    explicit ChainSink(fiber::mem::IoBufChain &out) noexcept : out_(out) {}
    [[nodiscard]] bool write(const char *data, std::size_t len) noexcept override {
        const auto room = static_cast<std::size_t>(end_ - cursor_);
        if (len == 0 || len > room) {
            return write_across_nodes(data, len);
        }
        if (len == 1) {
            *cursor_ = *data;
        } else {
            std::memcpy(cursor_, data, len);
        }
        cursor_ += len;
        return true;
    }
    void commit() noexcept {
        if (cursor_ != start_) {
            out_.commit_tailroom(static_cast<std::size_t>(cursor_ - start_));
        }
        start_ = nullptr;
        cursor_ = nullptr;
        end_ = nullptr;
    }
    [[nodiscard]] bool oom() const noexcept { return oom_; }

private:
    [[nodiscard]] bool write_across_nodes(const char *data, std::size_t len) noexcept {
        while (len != 0) {
            if (cursor_ == end_) {
                commit();
                if (!open_tail()) {
                    return false;
                }
            }
            const std::size_t room = std::min(len, static_cast<std::size_t>(end_ - cursor_));
            std::memcpy(cursor_, data, room);
            cursor_ += room;
            data += room;
            len -= room;
        }
        return true;
    }
    [[nodiscard]] bool open_tail() noexcept {
        fiber::mem::IoBuf *tail = out_.back();
        if (tail == nullptr || tail->tailroom() == 0) {
            fiber::mem::IoBuf fresh = fiber::mem::IoBuf::allocate(kJsonNodeCapacity);
            if (!fresh || !out_.append(std::move(fresh))) {
                oom_ = true;
                return false;
            }
            tail = out_.back();
        }
        start_ = reinterpret_cast<char *>(tail->writable_data());
        cursor_ = start_;
        end_ = start_ + tail->tailroom();
        return true;
    }

    fiber::mem::IoBufChain &out_;
    char *start_ = nullptr;
    char *cursor_ = nullptr;
    char *end_ = nullptr;
    bool oom_ = false;
};

// The error a failed JSON encode into sink is reported as: sink allocation failure is the
// transport-side failure source, value-shape encode errors keep the old Invalid mapping.
fiber::common::IoErr json_encode_error(const ChainSink &sink) noexcept {
    return sink.oom() ? fiber::common::IoErr::NoMem : fiber::common::IoErr::Invalid;
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

// form_decode_query's buffer source: exchange pool memory, released when the request ends.
auto query_alloc(fiber::mem::BufPool &pool) noexcept {
    return [&pool](std::size_t size) noexcept { return pool.alloc<char>(size); };
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
    auto query_io = fiber::util::form_decode_query(q, query_alloc(exchange_.pool()),
                                                   [&](std::string_view k, std::string_view v) noexcept -> bool {
                                                       return put_string(heap_, query_root_, k, v);
                                                   });
    if (!query_io && query_io.error() == fiber::common::IoErr::NoMem) {
        *query_root_ = fiber::script::JsValue::make_undefined(); // OOM; drop the partial object, retry next call
    }
    return *query_root_; // malformed percent escapes leave the partial object (lenient)
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
        constant_cache_size_ + 1 >
                constant_cache_capacity_ - (constant_cache_capacity_ / kConstantCacheMinFreeQuarter)) {
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
                    exchange_.uri().query, query_alloc(exchange_.pool()),
                    [&](std::string_view key, std::string_view value) noexcept -> bool {
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

fiber::http::OutgoingHeaderBlockView ScriptExchangeCtx::final_header(int status,
                                                                     fiber::http::HttpBodySpec body) const noexcept {
    fiber::http::OutgoingHeaderBlockView header{};
    header.kind = fiber::http::OutgoingHeaderKind::Final;
    header.status_code = status;
    header.headers = &pending_headers_;
    header.body = body;
    header.connection_mode = fiber::http::ResponseConnectionMode::Auto;
    header.end_stream = false;
    return header;
}

fiber::async::Task<fiber::common::IoResult<void>>
ScriptExchangeCtx::send_final_with_body(int status, std::size_t content_length, const std::uint8_t *data) noexcept {
    const fiber::http::OutgoingHeaderBlockView header =
            final_header(status, fiber::http::HttpBodySpec::ContentLength(content_length));
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
ScriptExchangeCtx::send_final_with_body(int status, fiber::mem::IoBufChain body) noexcept {
    const fiber::http::OutgoingHeaderBlockView header =
            final_header(status, fiber::http::HttpBodySpec::ContentLength(body.readable_bytes()));
    auto header_result = co_await response_writer_.send_header(header);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    header_sent_ = true;

    // complete() is the chain-path end-of-stream marker: HttpExchange::write_all maps it
    // to the same end flag the pointer overload takes explicitly.
    body.mark_complete();
    auto body_result = co_await response_writer_.write_all(std::move(body));
    if (!body_result) {
        co_return std::unexpected(body_result.error());
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::Task<fiber::common::IoResult<void>>
ScriptExchangeCtx::write_json(int status, const fiber::script::JsValue &body) noexcept {
    using Status = fiber::script::json::JsValueEncoder::Status;

    fiber::mem::IoBufChain json;
    ChainSink sink(json);
    fiber::json::Generator gen(sink);
    fiber::script::json::JsValueEncoder encoder(gen, body);
    Status encode_status = encoder.run(kJsonFlushBytes);
    if (encode_status == Status::Failed) {
        co_return std::unexpected(json_encode_error(sink));
    }
    sink.commit();
    set_response_header("Content-Type", "application/json");
    if (encode_status == Status::Done) {
        co_return co_await send_final_with_body(status, std::move(json));
    }

    const fiber::http::OutgoingHeaderBlockView header = final_header(status, fiber::http::HttpBodySpec::Stream());
    auto header_result = co_await response_writer_.send_header(header);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    header_sent_ = true;
    if (!exchange_.response_has_body(status)) {
        co_return fiber::common::IoResult<void>{}; // HEAD and the like: the rest would be thrown away.
    }

    // The encoder walks the heap between writes, which is safe only while nothing collects it.
    const std::uint64_t gc_count = heap_.stats().gc_count;
    for (;;) {
        const bool last = encode_status == Status::Done;
        if (last) {
            json.mark_complete();
        }
        auto body_result = co_await response_writer_.write_all(std::exchange(json, {}));
        if (!body_result) {
            co_return std::unexpected(body_result.error());
        }
        if (last) {
            co_return fiber::common::IoResult<void>{};
        }
        FIBER_ASSERT(heap_.stats().gc_count == gc_count);
        encode_status = encoder.run(kJsonFlushBytes);
        if (encode_status == Status::Failed) {
            // The status line is gone, so a broken body is the only way left to tell the client.
            // Returning from the handler would end the response too, but only once the script does.
            const fiber::common::IoErr err = json_encode_error(sink);
            (void) response_writer_.abort(err);
            co_return std::unexpected(err);
        }
        sink.commit();
    }
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
    fiber::mem::IoBufChain json;
    ChainSink sink(json);
    fiber::json::Generator gen(sink);
    using R = fiber::json::Generator::Result;
    // On this fixed shape the only failure source is the sink; the oom() flag still
    // separates allocation failure from generator error state for the error value.
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
    sink.commit();
    set_response_header("Content-Type", "application/json");
    co_return co_await send_final_with_body(status, std::move(json));
}

} // namespace fiber::http_script
