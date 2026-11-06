#include <fiber/common/util/CookieCodec.h>

#include <cstring>

namespace fiber::util {

namespace {

// RFC 6265 4.1.1 token-char set. A name must be non-empty and consist solely of these.
bool is_token_char(unsigned char c) noexcept {
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    if (c >= '0' && c <= '9') {
        return true;
    }
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

// cookie-octet excludes control, whitespace, DQUOTE, comma, semicolon, backslash.
bool needs_quoting(unsigned char c) noexcept {
    if (c <= 0x20 || c == 0x7F) {
        return true;
    }
    switch (c) {
        case '"':
        case ',':
        case ';':
        case '\\':
            return true;
        default:
            return false;
    }
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (char c: name) {
        if (!is_token_char(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

const char *same_site_text(CookieSameSite s) noexcept {
    switch (s) {
        case CookieSameSite::None:
            return "None";
        case CookieSameSite::Lax:
            return "Lax";
        case CookieSameSite::Strict:
            return "Strict";
        case CookieSameSite::Unset:
            return nullptr;
    }
    return nullptr;
}

} // namespace

namespace detail {

std::size_t cookie_encode(const Cookie &cookie, char *out) noexcept {
    if (!valid_name(cookie.name)) {
        return 0;
    }
    std::size_t n = 0;
    // Skips the copy while sizing (out == nullptr), so one walk serves both passes.
    const auto put = [&](std::string_view s) noexcept {
        if (out != nullptr && !s.empty()) {
            std::memcpy(out + n, s.data(), s.size());
        }
        n += s.size();
    };

    put(cookie.name);
    put("=");

    bool quote = false;
    for (char c: cookie.value) {
        if (needs_quoting(static_cast<unsigned char>(c))) {
            quote = true;
            break;
        }
    }
    if (!quote) {
        put(cookie.value);
    } else {
        put("\"");
        for (char c: cookie.value) {
            if (c == '"' || c == '\\') {
                put("\\");
            }
            put(std::string_view{&c, 1});
        }
        put("\"");
    }

    if (!cookie.domain.empty()) {
        put("; Domain=");
        put(cookie.domain);
    }
    if (!cookie.path.empty()) {
        put("; Path=");
        put(cookie.path);
    }
    if (cookie.max_age >= 0) {
        put("; Max-Age=");
        char buf[24];
        char *p = buf + sizeof(buf);
        std::uint64_t v = static_cast<std::uint64_t>(cookie.max_age);
        if (v == 0) {
            *--p = '0';
        } else {
            while (v != 0) {
                *--p = static_cast<char>('0' + (v % 10));
                v /= 10;
            }
        }
        put(std::string_view{p, static_cast<std::size_t>(buf + sizeof(buf) - p)});
    }
    if (cookie.secure) {
        put("; Secure");
    }
    if (cookie.http_only) {
        put("; HttpOnly");
    }
    if (const char *ss = same_site_text(cookie.same_site)) {
        put("; SameSite=");
        put(ss);
    }
    return n;
}

} // namespace detail

} // namespace fiber::util
