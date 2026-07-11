#include "CookieCodec.h"

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

void append_value(std::string &out, std::string_view value) {
    bool quote = false;
    for (char c: value) {
        if (needs_quoting(static_cast<unsigned char>(c))) {
            quote = true;
            break;
        }
    }
    if (!quote) {
        out.append(value.data(), value.size());
        return;
    }
    out.push_back('"');
    for (char c: value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
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

bool encode_set_cookie(const Cookie &cookie, std::string &out) {
    if (!valid_name(cookie.name)) {
        return false;
    }
    out.clear();
    out.append(cookie.name.data(), cookie.name.size());
    out.push_back('=');
    append_value(out, cookie.value);

    if (!cookie.domain.empty()) {
        out.append("; Domain=");
        out.append(cookie.domain.data(), cookie.domain.size());
    }
    if (!cookie.path.empty()) {
        out.append("; Path=");
        out.append(cookie.path.data(), cookie.path.size());
    }
    if (cookie.max_age >= 0) {
        out.append("; Max-Age=");
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
        out.append(p, static_cast<std::size_t>(buf + sizeof(buf) - p));
    }
    if (cookie.secure) {
        out.append("; Secure");
    }
    if (cookie.http_only) {
        out.append("; HttpOnly");
    }
    if (const char *ss = same_site_text(cookie.same_site)) {
        out.append("; SameSite=");
        out.append(ss);
    }
    return true;
}

} // namespace fiber::util
