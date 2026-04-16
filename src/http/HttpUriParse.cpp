#include "HttpUriParse.h"

#include <cstdint>
#include <limits>

namespace fiber::http {

namespace {

constexpr std::size_t kUriPosNpos = std::numeric_limits<std::size_t>::max();

constexpr std::uint32_t kUsualUriChar[8] = {
        0x00000000u,
        0x7fff37d6u,
#if defined(_WIN32)
        0xefffffffu,
#else
        0xffffffffu,
#endif
        0x7fffffffu,
        0xffffffffu,
        0xffffffffu,
        0xffffffffu,
        0xffffffffu,
};

enum class ScanState : std::uint8_t {
    Start,
    AfterSlash,
    CheckUri,
    Uri,
};

enum class ComplexState : std::uint8_t {
    Usual,
    Slash,
    Dot,
    DotDot,
    Quoted,
    QuotedSecond,
};

[[nodiscard]] bool is_usual_uri_char(unsigned char ch) noexcept {
    return (kUsualUriChar[ch >> 5] & (1U << (ch & 0x1f))) != 0;
}

unsigned char next_uri_char(std::string_view raw_uri, std::size_t &pos) noexcept {
    if (pos < raw_uri.size()) {
        return static_cast<unsigned char>(raw_uri[pos++]);
    }
    pos = raw_uri.size() + 1;
    return '\0';
}

void assign_simple_extension(std::string_view path, HttpUri &uri) noexcept {
    uri.exten = {};

    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) {
        return;
    }

    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string_view::npos && dot <= slash + 1) {
        return;
    }

    uri.exten = path.substr(dot + 1);
}

common::IoErr finalize_simple_uri(std::string_view raw_uri, HttpUri &uri) noexcept {
    uri.unparsed_uri = raw_uri;
    uri.path = raw_uri;
    uri.query = {};
    uri.exten = {};

    const std::size_t query_marker = raw_uri.find('?');
    if (query_marker != std::string_view::npos) {
        uri.path = raw_uri.substr(0, query_marker);
        uri.query = raw_uri.substr(query_marker + 1);
    }

    assign_simple_extension(uri.path, uri);
    return common::IoErr::None;
}

common::IoErr finalize_complex_uri(std::string_view raw_uri, const HttpUriParseState &state, HttpUri &uri,
                                   mem::BufPool *pool, bool merge_slashes) noexcept {
    if (!pool) {
        return common::IoErr::NoMem;
    }

    const std::size_t dst_cap = raw_uri.size() + (state.empty_path_in_uri ? 1u : 0u);
    char *dst = static_cast<char *>(pool->alloc(dst_cap));
    if (!dst && dst_cap != 0) {
        return common::IoErr::NoMem;
    }

    ComplexState current = ComplexState::Usual;
    ComplexState quoted_state = ComplexState::Usual;
    std::size_t src = 0;
    std::size_t out = 0;
    std::size_t exten_start = kUriPosNpos;
    std::size_t query_start = kUriPosNpos;
    std::size_t query_len = 0;
    bool query_len_set = false;
    unsigned char decoded = '\0';

    if (state.empty_path_in_uri) {
        dst[out++] = '/';
    }

    unsigned char ch = next_uri_char(raw_uri, src);
    while (src <= raw_uri.size()) {
        switch (current) {
            case ComplexState::Usual:
                if (is_usual_uri_char(ch)) {
                    dst[out++] = static_cast<char>(ch);
                    ch = next_uri_char(raw_uri, src);
                    break;
                }

                switch (ch) {
#if defined(_WIN32)
                    case '\\':
                        if (out >= 2 && dst[out - 1] == '.' && dst[out - 2] != '.') {
                            --out;
                        }
                        exten_start = kUriPosNpos;
                        if (src == raw_uri.size() + 1) {
                            break;
                        }
                        current = ComplexState::Slash;
                        dst[out++] = '/';
                        break;
#endif
                    case '/':
#if defined(_WIN32)
                        if (out >= 2 && dst[out - 1] == '.' && dst[out - 2] != '.') {
                            --out;
                        }
#endif
                        exten_start = kUriPosNpos;
                        current = ComplexState::Slash;
                        dst[out++] = '/';
                        break;
                    case '%':
                        quoted_state = current;
                        current = ComplexState::Quoted;
                        break;
                    case '?':
                        query_start = src;
                        goto args;
                    case '#':
                        goto done;
                    case '.':
                        exten_start = out + 1;
                        dst[out++] = '.';
                        break;
                    case '+':
                        dst[out++] = '+';
                        break;
                    default:
                        dst[out++] = static_cast<char>(ch);
                        break;
                }

                ch = next_uri_char(raw_uri, src);
                break;

            case ComplexState::Slash:
                if (is_usual_uri_char(ch)) {
                    current = ComplexState::Usual;
                    dst[out++] = static_cast<char>(ch);
                    ch = next_uri_char(raw_uri, src);
                    break;
                }

                switch (ch) {
#if defined(_WIN32)
                    case '\\':
                        break;
#endif
                    case '/':
                        if (!merge_slashes) {
                            dst[out++] = '/';
                        }
                        break;
                    case '.':
                        current = ComplexState::Dot;
                        dst[out++] = '.';
                        break;
                    case '%':
                        quoted_state = current;
                        current = ComplexState::Quoted;
                        break;
                    case '?':
                        query_start = src;
                        goto args;
                    case '#':
                        goto done;
                    case '+':
                        current = ComplexState::Usual;
                        dst[out++] = '+';
                        break;
                    default:
                        current = ComplexState::Usual;
                        dst[out++] = static_cast<char>(ch);
                        break;
                }

                ch = next_uri_char(raw_uri, src);
                break;

            case ComplexState::Dot:
                if (is_usual_uri_char(ch)) {
                    current = ComplexState::Usual;
                    dst[out++] = static_cast<char>(ch);
                    ch = next_uri_char(raw_uri, src);
                    break;
                }

                switch (ch) {
#if defined(_WIN32)
                    case '\\':
#endif
                    case '/':
                        current = ComplexState::Slash;
                        --out;
                        break;
                    case '.':
                        current = ComplexState::DotDot;
                        dst[out++] = '.';
                        break;
                    case '%':
                        quoted_state = current;
                        current = ComplexState::Quoted;
                        break;
                    case '?':
                        --out;
                        query_start = src;
                        goto args;
                    case '#':
                        --out;
                        goto done;
                    case '+':
                        current = ComplexState::Usual;
                        dst[out++] = '+';
                        break;
                    default:
                        current = ComplexState::Usual;
                        dst[out++] = static_cast<char>(ch);
                        break;
                }

                ch = next_uri_char(raw_uri, src);
                break;

            case ComplexState::DotDot:
                if (is_usual_uri_char(ch)) {
                    current = ComplexState::Usual;
                    dst[out++] = static_cast<char>(ch);
                    ch = next_uri_char(raw_uri, src);
                    break;
                }

                switch (ch) {
#if defined(_WIN32)
                    case '\\':
#endif
                    case '/':
                    case '?':
                    case '#':
                        if (out < 4) {
                            return common::IoErr::Invalid;
                        }
                        out -= 4;
                        for (;;) {
                            if (out == 0) {
                                return common::IoErr::Invalid;
                            }
                            if (dst[out - 1] == '/') {
                                break;
                            }
                            --out;
                        }
                        if (ch == '?') {
                            query_start = src;
                            goto args;
                        }
                        if (ch == '#') {
                            goto done;
                        }
                        current = ComplexState::Slash;
                        break;
                    case '%':
                        quoted_state = current;
                        current = ComplexState::Quoted;
                        break;
                    case '+':
                        current = ComplexState::Usual;
                        dst[out++] = '+';
                        break;
                    default:
                        current = ComplexState::Usual;
                        dst[out++] = static_cast<char>(ch);
                        break;
                }

                ch = next_uri_char(raw_uri, src);
                break;

            case ComplexState::Quoted:
                if (ch >= '0' && ch <= '9') {
                    decoded = static_cast<unsigned char>(ch - '0');
                    current = ComplexState::QuotedSecond;
                    ch = next_uri_char(raw_uri, src);
                    break;
                }

                {
                    const unsigned char lower = static_cast<unsigned char>(ch | 0x20);
                    if (lower >= 'a' && lower <= 'f') {
                        decoded = static_cast<unsigned char>(lower - 'a' + 10);
                        current = ComplexState::QuotedSecond;
                        ch = next_uri_char(raw_uri, src);
                        break;
                    }
                }

                return common::IoErr::Invalid;

            case ComplexState::QuotedSecond:
                if (ch >= '0' && ch <= '9') {
                    ch = static_cast<unsigned char>((decoded << 4) + (ch - '0'));

                    if (ch == '%' || ch == '#') {
                        current = ComplexState::Usual;
                        dst[out++] = static_cast<char>(ch);
                        ch = next_uri_char(raw_uri, src);
                        break;
                    }
                    if (ch == '\0') {
                        return common::IoErr::Invalid;
                    }

                    current = quoted_state;
                    break;
                }

                {
                    const unsigned char lower = static_cast<unsigned char>(ch | 0x20);
                    if (lower >= 'a' && lower <= 'f') {
                        ch = static_cast<unsigned char>((decoded << 4) + (lower - 'a') + 10);

                        if (ch == '?') {
                            current = ComplexState::Usual;
                            dst[out++] = static_cast<char>(ch);
                            ch = next_uri_char(raw_uri, src);
                            break;
                        }

                        current = quoted_state;
                        break;
                    }
                }

                return common::IoErr::Invalid;
        }
    }

    if (current == ComplexState::Quoted || current == ComplexState::QuotedSecond) {
        return common::IoErr::Invalid;
    }

    if (current == ComplexState::Dot) {
        --out;
    } else if (current == ComplexState::DotDot) {
        if (out < 4) {
            return common::IoErr::Invalid;
        }
        out -= 4;
        for (;;) {
            if (out == 0) {
                return common::IoErr::Invalid;
            }
            if (dst[out - 1] == '/') {
                break;
            }
            --out;
        }
    }

done:
    uri.unparsed_uri = raw_uri;
    uri.path = std::string_view(dst, out);
    uri.query = {};
    uri.exten = {};
    if (exten_start != kUriPosNpos && exten_start <= out) {
        uri.exten = std::string_view(dst + exten_start, out - exten_start);
    }
    return common::IoErr::None;

args:
    while (src < raw_uri.size()) {
        if (raw_uri[src++] != '#') {
            continue;
        }
        query_len = (src - 1) - query_start;
        query_len_set = true;
        break;
    }

    uri.unparsed_uri = raw_uri;
    uri.path = std::string_view(dst, out);
    uri.query = {};
    uri.exten = {};
    if (exten_start != kUriPosNpos && exten_start <= out) {
        uri.exten = std::string_view(dst + exten_start, out - exten_start);
    }
    if (query_start != kUriPosNpos) {
        const std::size_t len = query_len_set ? query_len : raw_uri.size() - query_start;
        uri.query = raw_uri.substr(query_start, len);
    }
    return common::IoErr::None;
}

} // namespace

common::IoErr http_scan_origin_form_uri(std::string_view raw_uri, HttpUriParseState &state) noexcept {
    state = {};

    if (raw_uri.empty()) {
        return common::IoErr::Invalid;
    }

    ScanState current = ScanState::Start;

    for (std::size_t i = 0; i < raw_uri.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw_uri[i]);

        switch (current) {
            case ScanState::Start:
                if (ch != '/') {
                    return common::IoErr::Invalid;
                }
                current = ScanState::AfterSlash;
                break;

            case ScanState::AfterSlash:
                if (is_usual_uri_char(ch)) {
                    current = ScanState::CheckUri;
                    break;
                }

                switch (ch) {
                    case '.':
                        state.complex_uri = true;
                        current = ScanState::Uri;
                        break;
                    case '%':
                        state.quoted_uri = true;
                        current = ScanState::Uri;
                        break;
                    case '/':
                        state.complex_uri = true;
                        current = ScanState::Uri;
                        break;
#if defined(_WIN32)
                    case '\\':
                        state.complex_uri = true;
                        current = ScanState::Uri;
                        break;
#endif
                    case '?':
                        current = ScanState::Uri;
                        break;
                    case '#':
                        state.complex_uri = true;
                        current = ScanState::Uri;
                        break;
                    case '+':
                        state.plus_in_uri = true;
                        break;
                    default:
                        if (ch <= 0x20 || ch == 0x7f) {
                            return common::IoErr::Invalid;
                        }
                        current = ScanState::CheckUri;
                        break;
                }
                break;

            case ScanState::CheckUri:
                if (is_usual_uri_char(ch)) {
                    break;
                }

                switch (ch) {
                    case '/':
#if defined(_WIN32)
                        current = ScanState::Uri;
                        state.complex_uri = true;
                        break;
#else
                        current = ScanState::AfterSlash;
                        break;
#endif
                    case '.':
                        break;
#if defined(_WIN32)
                    case '\\':
                        state.complex_uri = true;
                        current = ScanState::AfterSlash;
                        break;
#endif
                    case '%':
                        state.quoted_uri = true;
                        current = ScanState::Uri;
                        break;
                    case '?':
                        current = ScanState::Uri;
                        break;
                    case '#':
                        state.complex_uri = true;
                        current = ScanState::Uri;
                        break;
                    case '+':
                        state.plus_in_uri = true;
                        break;
                    default:
                        if (ch <= 0x20 || ch == 0x7f) {
                            return common::IoErr::Invalid;
                        }
                        break;
                }
                break;

            case ScanState::Uri:
                if (is_usual_uri_char(ch)) {
                    break;
                }

                switch (ch) {
                    case '#':
                        state.complex_uri = true;
                        break;
                    default:
                        if (ch <= 0x20 || ch == 0x7f) {
                            return common::IoErr::Invalid;
                        }
                        break;
                }
                break;
        }
    }

    return common::IoErr::None;
}

common::IoErr http_finalize_request_uri(std::string_view raw_uri, const HttpUriParseState &state, HttpUri &uri,
                                        mem::BufPool *pool, bool merge_slashes) noexcept {
    if (state.complex_uri || state.quoted_uri || state.empty_path_in_uri) {
        return finalize_complex_uri(raw_uri, state, uri, pool, merge_slashes);
    }

    return finalize_simple_uri(raw_uri, uri);
}

} // namespace fiber::http
