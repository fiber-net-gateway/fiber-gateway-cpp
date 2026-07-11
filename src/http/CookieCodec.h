#ifndef FIBER_HTTP_COOKIE_CODEC_H
#define FIBER_HTTP_COOKIE_CODEC_H

#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::http {

// RFC 6265 SameSite attribute values. Unset mirrors Netty's null SameSite (attribute
// omitted); None/Lax/Strict map to the literal "SameSite=None|Lax|Strict".
enum class CookieSameSite : std::uint8_t {
    Unset = 0,
    None,
    Lax,
    Strict,
};

// A cookie as assembled by resp.addCookie. Mirrors the subset of Netty's DefaultCookie
// fields that the script-facing addCookie exposes (name/value/domain/path/maxAge/secure/
// httpOnly/sameSite). All string fields are borrowed; the caller guarantees lifetime
// across encode().
struct Cookie {
    std::string_view name;
    std::string_view value;
    std::string_view domain;
    std::string_view path;
    // -1 == session cookie (no Max-Age attribute), matching Netty Cookie.UNDEFINED_MAX_AGE.
    std::int64_t max_age = -1;
    bool secure = false;
    bool http_only = false;
    CookieSameSite same_site = CookieSameSite::Unset;
};

// Splits a request "Cookie:" header value into (name, value) pairs, invoking sink once
// per pair. Pairs are separated by ';' (optional surrounding spaces are trimmed); the
// first '=' splits name/value. A segment without '=' yields (segment, ""). Empty
// segments are skipped. Mirrors Netty ServerCookieDecoder (lax) for the Cookie header.
// sink is called as sink(std::string_view name, std::string_view value); its return is
// ignored (decoding always visits every pair).
template<typename Sink>
void decode_cookie_header(std::string_view header, Sink &&sink);

template<typename Sink>
void decode_cookie_header(std::string_view header, Sink &&sink) {
    const std::size_t len = header.size();
    std::size_t pos = 0;
    while (pos <= len) {
        std::size_t seg_end = pos;
        while (seg_end < len && header[seg_end] != ';') {
            ++seg_end;
        }
        std::string_view seg = header.substr(pos, seg_end - pos);
        // Trim spaces around the pair (Netty tolerates "; name=value").
        while (!seg.empty() && (seg.front() == ' ' || seg.front() == '\t')) {
            seg.remove_prefix(1);
        }
        while (!seg.empty() && (seg.back() == ' ' || seg.back() == '\t')) {
            seg.remove_suffix(1);
        }
        if (!seg.empty()) {
            std::size_t eq = seg.find('=');
            std::string_view name = eq == std::string_view::npos ? seg : seg.substr(0, eq);
            std::string_view value = eq == std::string_view::npos ? std::string_view{} : seg.substr(eq + 1);
            sink(name, value);
        }
        pos = seg_end + 1;
    }
}

// Encodes a cookie into a "Set-Cookie" header value (without the field name). Returns
// false (and leaves out empty) when the name is empty or contains an invalid token char,
// matching Netty ServerCookieEncoder.STRICT raising on invalid names (resp.addCookie
// then yields false). On success, out holds "name=value; Domain=...; ...".
[[nodiscard]] bool encode_set_cookie(const Cookie &cookie, std::string &out);

} // namespace fiber::http

#endif // FIBER_HTTP_COOKIE_CODEC_H
