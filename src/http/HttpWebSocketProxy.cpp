#include <fiber/http/HttpWebSocketProxy.h>

#include <array>
#include <string_view>
#include <utility>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/IoError.h>
#include <fiber/common/util/Base64.h>
#include <fiber/http/ClientHttp1Exchange.h>
#include <fiber/http/ClientHttp1Types.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/HttpProxyCore.h>

namespace fiber::http::proxy_core {
namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool header_has_token(const HttpHeaders &headers, std::string_view name, std::uint64_t name_hash,
                      std::string_view expected) noexcept {
    for (const auto &field: headers.get_all(name, name_hash)) {
        bool matched = false;
        for_each_token(field.value_view(), [&](std::string_view token) {
            if (!matched && http_header_name_equals_ci(token, expected)) {
                matched = true;
            }
        });
        if (matched) {
            return true;
        }
    }
    return false;
}

// SHA1(key || GUID) base64-encoded into the fixed accept buffer. Allocation-free:
// the 28-char output length is a compile-time constant.
bool build_expected_websocket_accept(std::string_view key, std::array<char, kWebSocketAcceptB64Len> &out) noexcept {
    static_assert(fiber::util::base64_encoded_size(SHA_DIGEST_LENGTH) == kWebSocketAcceptB64Len);
    if (key.empty()) {
        return false;
    }
    SHA_CTX ctx;
    std::array<std::uint8_t, SHA_DIGEST_LENGTH> digest{};
    if (SHA1_Init(&ctx) != 1 || SHA1_Update(&ctx, key.data(), key.size()) != 1 ||
        SHA1_Update(&ctx, kWebSocketGuid.data(), kWebSocketGuid.size()) != 1 || SHA1_Final(digest.data(), &ctx) != 1) {
        return false;
    }
    fiber::util::base64_encode_into(digest.data(), digest.size(), out.data());
    return true;
}

struct WebSocketTunnelState {
    HttpExchange *downstream = nullptr;
    ClientHttp1Exchange *upstream = nullptr;
    bool stopped = false;

    void stop(fiber::common::IoErr reason) noexcept {
        if (stopped) {
            return;
        }
        stopped = true;
        if (downstream != nullptr) {
            (void) downstream->abort(reason);
        }
        if (upstream != nullptr && upstream->valid()) {
            (void) upstream->abort(reason);
        }
    }
};

fiber::async::Task<fiber::common::IoErr> relay_websocket_downlink(HttpExchange &downstream,
                                                                  ClientHttp1Exchange &upstream,
                                                                  std::chrono::milliseconds read_timeout,
                                                                  std::chrono::milliseconds write_timeout) noexcept {
    for (;;) {
        auto body_result = co_await upstream.read_body(kBodyChunkSize, read_timeout);
        if (!body_result) {
            co_return body_result.error();
        }
        const bool last = body_result->complete();
        auto write_result = co_await downstream.write_all(std::move(*body_result), write_timeout);
        if (!write_result) {
            co_return write_result.error();
        }
        if (last) {
            co_return fiber::common::IoErr::Canceled;
        }
    }
}

fiber::async::Task<fiber::common::IoErr> relay_websocket_uplink(HttpExchange &downstream, ClientHttp1Exchange &upstream,
                                                                std::chrono::milliseconds write_timeout) noexcept {
    for (;;) {
        auto body_result = co_await downstream.read_body(kBodyChunkSize);
        if (!body_result) {
            co_return body_result.error();
        }
        const bool last = body_result->complete();
        auto write_result = co_await upstream.write_all(std::move(*body_result), write_timeout);
        if (!write_result) {
            co_return write_result.error();
        }
        if (last) {
            co_return fiber::common::IoErr::Canceled;
        }
    }
}

} // namespace

WebSocketDownstream detect_websocket_downstream(const HttpExchange &exchange) noexcept {
    if ((exchange.version() == HttpVersion::HTTP_2_0 || exchange.version() == HttpVersion::HTTP_3_0) &&
        exchange.method() == HttpMethod::Connect && http_header_name_equals_ci(exchange.protocol(), "websocket")) {
        return WebSocketDownstream::ExtendedConnect;
    }

    static constexpr std::uint64_t kConnectionHash = http_header_name_hash("connection");
    static constexpr std::uint64_t kUpgradeHash = http_header_name_hash("upgrade");
    if (exchange.version() == HttpVersion::HTTP_1_1 && exchange.method() == HttpMethod::Get &&
        header_has_token(exchange.request_headers(), "connection", kConnectionHash, "upgrade") &&
        header_has_token(exchange.request_headers(), "upgrade", kUpgradeHash, "websocket")) {
        return WebSocketDownstream::Http1Upgrade;
    }
    return WebSocketDownstream::None;
}

bool prepare_websocket_handshake(WebSocketHandshake &handshake) noexcept {
    if (!handshake.extended_connect()) {
        return true;
    }
    std::array<std::uint8_t, 16> random{};
    if (RAND_bytes(random.data(), random.size()) != 1) {
        return false;
    }
    static_assert(fiber::util::base64_encoded_size(16) == kWebSocketKeyB64Len);
    fiber::util::base64_encode_into(random.data(), random.size(), handshake.generated_key.data());
    handshake.key_generated = true;
    return true;
}

bool prepare_upstream_websocket_headers(const HttpExchange &exchange, WebSocketHandshake &handshake,
                                        HttpHeaders &headers) noexcept {
    if (!handshake.active()) {
        return false;
    }

    static constexpr std::uint64_t kKeyHash = http_header_name_hash("sec-websocket-key");
    static constexpr std::uint64_t kAcceptHash = http_header_name_hash("sec-websocket-accept");
    static constexpr std::uint64_t kVersionHash = http_header_name_hash("sec-websocket-version");

    headers.remove("sec-websocket-accept", kAcceptHash);
    if (headers.set("Connection", "Upgrade") == nullptr || headers.set("Upgrade", "websocket") == nullptr) {
        return false;
    }

    std::string_view key;
    if (handshake.extended_connect()) {
        if (!handshake.key_generated || headers.set("Sec-WebSocket-Key", handshake.key_view()) == nullptr ||
            headers.set("Sec-WebSocket-Version", "13") == nullptr) {
            return false;
        }
        key = handshake.key_view();
    } else {
        key = trim_lws(exchange.request_headers().get("sec-websocket-key", kKeyHash));
        if (key.empty() || headers.set("Sec-WebSocket-Key", key) == nullptr) {
            return false;
        }
        const std::string_view version =
                trim_lws(exchange.request_headers().get("sec-websocket-version", kVersionHash));
        if (!version.empty() && headers.set("Sec-WebSocket-Version", version) == nullptr) {
            return false;
        }
    }

    if (!build_expected_websocket_accept(key, handshake.expected_accept)) {
        return false;
    }
    handshake.accept_ready = true;
    return true;
}

bool valid_websocket_upgrade_response(const Http1ResponseHead &head, const WebSocketHandshake &handshake) noexcept {
    static constexpr std::uint64_t kConnectionHash = http_header_name_hash("connection");
    static constexpr std::uint64_t kUpgradeHash = http_header_name_hash("upgrade");
    static constexpr std::uint64_t kAcceptHash = http_header_name_hash("sec-websocket-accept");
    return handshake.active() && head.status_code == 101 &&
           header_has_token(head.headers, "connection", kConnectionHash, "upgrade") &&
           header_has_token(head.headers, "upgrade", kUpgradeHash, "websocket") &&
           trim_lws(head.headers.get("sec-websocket-accept", kAcceptHash)) == handshake.accept_view();
}

void build_downstream_websocket_headers(const Http1ResponseHead &upstream_head, HttpHeaders &headers,
                                        const WebSocketHandshake &handshake) noexcept {
    static constexpr std::uint64_t kAcceptHash = http_header_name_hash("sec-websocket-accept");

    for (const HttpHeaders::HeaderField &field: upstream_head.headers) {
        if (field.name_len == 0 || should_skip_hop_by_hop_header(upstream_head.headers, field)) {
            continue;
        }
        if (handshake.extended_connect() && field.name_hash == kAcceptHash &&
            field.lowcase_view() == "sec-websocket-accept") {
            continue;
        }
        headers.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash);
    }

    finalize_downstream_websocket_headers(headers, handshake);
}

void finalize_downstream_websocket_headers(HttpHeaders &headers, const WebSocketHandshake &handshake) noexcept {
    static constexpr std::uint64_t kConnectionHash = http_header_name_hash("connection");
    static constexpr std::uint64_t kUpgradeHash = http_header_name_hash("upgrade");
    static constexpr std::uint64_t kAcceptHash = http_header_name_hash("sec-websocket-accept");

    if (handshake.downstream == WebSocketDownstream::Http1Upgrade) {
        headers.set("Connection", "Upgrade");
        headers.set("Upgrade", "websocket");
        headers.set("Sec-WebSocket-Accept", handshake.accept_view());
        return;
    }

    headers.remove("connection", kConnectionHash);
    headers.remove("upgrade", kUpgradeHash);
    headers.remove("sec-websocket-accept", kAcceptHash);
}

fiber::async::Task<void> relay_websocket_tunnel(HttpExchange &downstream, ClientHttp1Exchange &upstream,
                                                std::chrono::milliseconds read_timeout,
                                                std::chrono::milliseconds write_timeout) noexcept {
    WebSocketTunnelState state{
            .downstream = &downstream,
            .upstream = &upstream,
    };

    auto completed = co_await fiber::async::when_any(
            [&]() { return relay_websocket_downlink(downstream, upstream, read_timeout, write_timeout).select(); },
            [&]() { return relay_websocket_uplink(downstream, upstream, write_timeout).select(); });
    fiber::common::IoErr reason;
    if (completed.is<0>()) {
        reason = std::move(completed).get<0>();
    } else {
        reason = std::move(completed).get<1>();
    }
    state.stop(reason);
}

} // namespace fiber::http::proxy_core
