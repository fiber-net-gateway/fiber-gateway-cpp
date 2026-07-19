#ifndef FIBER_HTTP_PROXY_CORE_H
#define FIBER_HTTP_PROXY_CORE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../common/IoError.h"
#include "HeaderMap.h"
#include "HttpBodySpec.h"
#include "HttpCommon.h"
#include "HttpExchange.h"
#include "HttpHeaderHash.h"
#include "HttpHeaders.h"

namespace fiber::http::proxy_core {

constexpr std::string_view kBadGatewayBody = "502 Bad Gateway\n";
constexpr std::string_view kGatewayTimeoutBody = "504 Gateway Timeout\n";
constexpr std::size_t kBodyChunkSize = 64 * 1024;
constexpr std::uint8_t kSkipHeaderValue = 1;

inline bool parse_decimal(std::string_view text, std::size_t &value) {
    if (text.empty()) {
        return false;
    }
    std::size_t out = 0;
    for (const unsigned char ch: text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        out = out * 10 + static_cast<std::size_t>(ch - '0');
    }
    value = out;
    return true;
}

inline std::string_view trim_lws(std::string_view value) noexcept {
    while (!value.empty()) {
        const char ch = value.front();
        if (ch != ' ' && ch != '\t') {
            break;
        }
        value.remove_prefix(1);
    }
    while (!value.empty()) {
        const char ch = value.back();
        if (ch != ' ' && ch != '\t') {
            break;
        }
        value.remove_suffix(1);
    }
    return value;
}

template<typename F>
void for_each_token(std::string_view value, F &&fn) {
    std::size_t start = 0;
    while (start < value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
        const std::string_view token = trim_lws(value.substr(start, end - start));
        if (!token.empty()) {
            fn(token);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
}

// Hop-by-hop headers that must not be forwarded between client and upstream (RFC 7230 6.1) plus
// the proxy-specific Connection/Keep-Alive/TE/Trailers/Transfer-Encoding/Upgrade family.
inline const fiber::http::HeaderMap<std::uint8_t> &hop_by_hop_header_map() {
    static const fiber::http::HeaderMap<std::uint8_t> map = []() {
        fiber::http::HeaderMap<std::uint8_t> headers;
        headers.insert("connection", fiber::http::http_header_name_hash("connection"), kSkipHeaderValue);
        headers.insert("keep-alive", fiber::http::http_header_name_hash("keep-alive"), kSkipHeaderValue);
        headers.insert("proxy-connection", fiber::http::http_header_name_hash("proxy-connection"), kSkipHeaderValue);
        headers.insert("transfer-encoding", fiber::http::http_header_name_hash("transfer-encoding"), kSkipHeaderValue);
        headers.insert("upgrade", fiber::http::http_header_name_hash("upgrade"), kSkipHeaderValue);
        headers.insert("te", fiber::http::http_header_name_hash("te"), kSkipHeaderValue);
        headers.insert("trailer", fiber::http::http_header_name_hash("trailer"), kSkipHeaderValue);
        headers.insert("proxy-authenticate", fiber::http::http_header_name_hash("proxy-authenticate"),
                       kSkipHeaderValue);
        headers.insert("proxy-authorization", fiber::http::http_header_name_hash("proxy-authorization"),
                       kSkipHeaderValue);
        return headers;
    }();
    return map;
}

inline bool connection_declares_hop_by_hop(const fiber::http::HttpHeaders &headers,
                                           std::string_view lowcase_name) noexcept {
    static constexpr std::uint64_t kConnectionHash = fiber::http::http_header_name_hash("connection");
    for (const auto &field: headers.get_all("connection", kConnectionHash)) {
        bool matched = false;
        for_each_token(field.value_view(), [&](std::string_view token) {
            if (!matched && fiber::http::http_header_name_equals_ci(token, lowcase_name)) {
                matched = true;
            }
        });
        if (matched) {
            return true;
        }
    }
    return false;
}

// A header is skipped if it is hop-by-hop (static set) or named in the message's own Connection
// header. Symmetric: usable for both request->upstream and upstream->response copying.
inline bool should_skip_hop_by_hop_header(const fiber::http::HttpHeaders &headers,
                                          const fiber::http::HttpHeaders::HeaderField &field) noexcept {
    if (hop_by_hop_header_map().get(field.lowcase_view(), field.name_hash)) {
        return true;
    }
    return connection_declares_hop_by_hop(headers, field.lowcase_view());
}

inline bool is_request_framing_header(const fiber::http::HttpHeaders::HeaderField &field) noexcept {
    static constexpr std::uint64_t kContentLengthHash = fiber::http::http_header_name_hash("content-length");
    static constexpr std::uint64_t kTransferEncodingHash = fiber::http::http_header_name_hash("transfer-encoding");
    return (field.name_hash == kContentLengthHash && field.lowcase_view() == "content-length") ||
           (field.name_hash == kTransferEncodingHash && field.lowcase_view() == "transfer-encoding");
}

inline void remove_request_framing_headers(fiber::http::HttpHeaders &headers) noexcept {
    static constexpr std::uint64_t kContentLengthHash = fiber::http::http_header_name_hash("content-length");
    static constexpr std::uint64_t kTransferEncodingHash = fiber::http::http_header_name_hash("transfer-encoding");
    headers.remove("content-length", kContentLengthHash);
    headers.remove("transfer-encoding", kTransferEncodingHash);
}

inline bool response_has_no_body(fiber::http::HttpMethod request_method, int status_code) noexcept {
    if (request_method == fiber::http::HttpMethod::Head) {
        return true;
    }
    return (status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 304;
}

inline std::string_view request_target_view(const fiber::http::HttpUri &uri, std::string &scratch) {
    if (!uri.unparsed_uri.empty()) {
        return uri.unparsed_uri;
    }
    if (uri.query.empty()) {
        return uri.path;
    }
    scratch.assign(uri.path.data(), uri.path.size());
    scratch.push_back('?');
    scratch.append(uri.query.data(), uri.query.size());
    return scratch;
}

inline int map_upstream_error_status(fiber::common::IoErr err) noexcept {
    return err == fiber::common::IoErr::TimedOut ? 504 : 502;
}

inline std::string_view map_upstream_error_body(fiber::common::IoErr err) noexcept {
    return err == fiber::common::IoErr::TimedOut ? kGatewayTimeoutBody : kBadGatewayBody;
}

// Returns the normalized inbound request body semantics for forwarding.
// end_stream=true when there is no body to forward (so send_header can close the request).
inline fiber::http::HttpBodySpec detect_request_body(const fiber::http::HttpExchange &exchange,
                                                     bool &end_stream) noexcept {
    const fiber::http::HttpBodySpec body = exchange.request_body_spec();
    if (body.is_content_length()) {
        end_stream = body.content_length() == 0;
        return fiber::http::HttpBodySpec::ContentLength(body.content_length());
    }
    if (body.is_none()) {
        end_stream = true;
        return fiber::http::HttpBodySpec::None();
    }
    // The current proxy upstream is HTTP/1, so any inbound body without a known
    // length is forwarded with HTTP/1 chunked transfer coding.
    end_stream = false;
    return fiber::http::HttpBodySpec::Chunked();
}

// A Content-Length upstream finishes as soon as its declared byte count has been
// written.  Some downstream protocols can report the transport FIN in a later,
// empty read, so that empty completion marker must not be written to the already
// completed upstream exchange.
class RequestBodyForwardState {
public:
    explicit RequestBodyForwardState(fiber::http::HttpBodySpec body) noexcept :
        content_length_(body.is_content_length()), remaining_(content_length_ ? body.content_length() : 0) {}

    [[nodiscard]] bool accepts(std::size_t bytes) const noexcept { return !content_length_ || bytes <= remaining_; }

    [[nodiscard]] bool should_write(std::size_t bytes) const noexcept {
        return !content_length_ || bytes != 0 || !upstream_complete_;
    }

    void record_write(std::size_t bytes) noexcept {
        if (content_length_) {
            remaining_ -= bytes;
            upstream_complete_ = remaining_ == 0;
        }
    }

    [[nodiscard]] bool complete() const noexcept { return upstream_complete_; }

private:
    bool content_length_ = false;
    bool upstream_complete_ = false;
    std::size_t remaining_ = 0;
};

} // namespace fiber::http::proxy_core

#endif // FIBER_HTTP_PROXY_CORE_H
