#include <cstring>
#include <fiber/http/Http1Parser.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpServerOptions.h>
#include <string_view>

namespace fiber::http {

namespace detail {

static constexpr uint32_t kUsual[8] = {0x00000000u, 0x7fff37d6u,
#if defined(_WIN32)
                                       0xefffffffu,
#else
                                       0xffffffffu,
#endif
                                       0x7fffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};

static const unsigned char kLowcase[] = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                        "\0\0\0\0\0\0\0\0\0\0\0\0\0-\0\0"
                                        "0123456789\0\0\0\0\0\0"
                                        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
                                        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
                                        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

unsigned char ascii_lower(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch + ('a' - 'A'));
    }
    return ch;
}

bool equals_ascii_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(static_cast<unsigned char>(a[i])) != ascii_lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool has_token(std::string_view value, std::string_view token) {
    size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == ',')) {
            ++i;
        }
        size_t start = i;
        while (i < value.size() && value[i] != ',') {
            ++i;
        }
        size_t end = i;
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
            --end;
        }
        if (end > start && equals_ascii_ci(value.substr(start, end - start), token)) {
            return true;
        }
        if (i < value.size() && value[i] == ',') {
            ++i;
        }
    }
    return false;
}

HttpMethod parse_method(const std::uint8_t *m, size_t len) {
    if (!m) {
        return HttpMethod::Unknown;
    }
    switch (len) {
        case 3:
            if (m[0] == 'G' && m[1] == 'E' && m[2] == 'T') {
                return HttpMethod::Get;
            }
            if (m[0] == 'P' && m[1] == 'U' && m[2] == 'T') {
                return HttpMethod::Put;
            }
            break;
        case 4:
            if (m[0] == 'P' && m[1] == 'O' && m[2] == 'S' && m[3] == 'T') {
                return HttpMethod::Post;
            }
            if (m[0] == 'H' && m[1] == 'E' && m[2] == 'A' && m[3] == 'D') {
                return HttpMethod::Head;
            }
            if (m[0] == 'C' && m[1] == 'O' && m[2] == 'P' && m[3] == 'Y') {
                return HttpMethod::Copy;
            }
            if (m[0] == 'M' && m[1] == 'O' && m[2] == 'V' && m[3] == 'E') {
                return HttpMethod::Move;
            }
            if (m[0] == 'L' && m[1] == 'O' && m[2] == 'C' && m[3] == 'K') {
                return HttpMethod::Lock;
            }
            break;
        case 5:
            if (m[0] == 'M' && m[1] == 'K' && m[2] == 'C' && m[3] == 'O' && m[4] == 'L') {
                return HttpMethod::MKCOL;
            }
            if (m[0] == 'P' && m[1] == 'A' && m[2] == 'T' && m[3] == 'C' && m[4] == 'H') {
                return HttpMethod::Patch;
            }
            if (m[0] == 'T' && m[1] == 'R' && m[2] == 'A' && m[3] == 'C' && m[4] == 'E') {
                return HttpMethod::Trace;
            }
            break;
        case 6:
            if (m[0] == 'D' && m[1] == 'E' && m[2] == 'L' && m[3] == 'E' && m[4] == 'T' && m[5] == 'E') {
                return HttpMethod::Delete;
            }
            if (m[0] == 'U' && m[1] == 'N' && m[2] == 'L' && m[3] == 'O' && m[4] == 'C' && m[5] == 'K') {
                return HttpMethod::Unlock;
            }
            break;
        case 7:
            if (m[0] == 'O' && m[1] == 'P' && m[2] == 'T' && m[3] == 'I' && m[4] == 'O' && m[5] == 'N' && m[6] == 'S') {
                return HttpMethod::Options;
            }
            if (m[0] == 'C' && m[1] == 'O' && m[2] == 'N' && m[3] == 'N' && m[4] == 'E' && m[5] == 'C' && m[6] == 'T') {
                return HttpMethod::Connect;
            }
            break;
        case 8:
            if (m[0] == 'P' && m[1] == 'R' && m[2] == 'O' && m[3] == 'P' && m[4] == 'F' && m[5] == 'I' && m[6] == 'N' &&
                m[7] == 'D') {
                return HttpMethod::PropFind;
            }
            break;
        case 9:
            if (m[0] == 'P' && m[1] == 'R' && m[2] == 'O' && m[3] == 'P' && m[4] == 'P' && m[5] == 'A' && m[6] == 'T' &&
                m[7] == 'C' && m[8] == 'H') {
                return HttpMethod::PropPatch;
            }
            break;
        default:
            break;
    }
    return HttpMethod::Unknown;
}

HttpVersion to_http_version(int version) {
    switch (version) {
        case 9:
            return HttpVersion::HTTP_0_9;
        case 1000:
            return HttpVersion::HTTP_1_0;
        case 1001:
            return HttpVersion::HTTP_1_1;
        case 3000:
            return HttpVersion::HTTP_3_0;
        default:
            return static_cast<HttpVersion>(version);
    }
}

} // namespace detail

RequestLineParser::RequestLineParser(const HttpServerOptions &options) : options_(&options) {}

void RequestLineParser::reset() {
    state_ = State::Start;
    line_ = RequestLineState{};
}

ParseCode RequestLineParser::replace_buf_ptr(mem::IoBuf *old_chain, mem::IoBuf *new_chain) noexcept {
    FIBER_ASSERT(state_ != State::Start);
    FIBER_ASSERT(old_chain->readable_data() > line_.request_start && old_chain->data() <= line_.request_start);
    std::uint8_t *new_buf_start = new_chain->writable_data();
    std::uint8_t *old = line_.request_start;
    auto length = static_cast<std::size_t>(old_chain->readable_data() - old);
    if (new_chain->writable() < length) {
        return ParseCode::HeaderTooLarge;
    }
    FIBER_ASSERT(length > 0);
    ::memcpy(new_buf_start, old, length);
    new_chain->commit(length);
    new_chain->consume(length);
    auto delta = new_buf_start - old;
    auto shift = [=](std::uint8_t *&ptr) {
        if (ptr) {
            ptr += delta;
        }
    };
    shift(line_.uri_start);
    shift(line_.uri_end);
    shift(line_.uri_ext);
    shift(line_.args_start);
    shift(line_.request_start);
    shift(line_.request_end);
    shift(line_.method_end);
    shift(line_.schema_start);
    shift(line_.schema_end);
    shift(line_.host_start);
    shift(line_.host_end);
    shift(line_.http_protocol_start);
    return ParseCode::Ok;
}

ParseCode RequestLineParser::execute(mem::IoBuf *buffer) {
    if (!buffer || buffer->readable() == 0) {
        return ParseCode::Again;
    }

    auto *begin = buffer->readable_data();
    auto *p = begin;
    auto *end = buffer->writable_data();
    State state = state_;

    for (; p < end; ++p) {
        unsigned char ch = *p;
        unsigned char c;
        switch (state) {
            case State::Start:
                line_.request_start = p;
                if (ch == '\r' || ch == '\n') {
                    break;
                }
                if ((ch < 'A' || ch > 'Z') && ch != '_' && ch != '-') {
                    return ParseCode::InvalidMethod;
                }
                state = State::Method;
                break;

            case State::Method:
                if (ch == ' ') {
                    line_.method_end = p - 1;
                    size_t len = static_cast<size_t>(p - line_.request_start);
                    line_.method = detail::parse_method(line_.request_start, len);
                    state = State::SpacesBeforeUri;
                    break;
                }
                if ((ch < 'A' || ch > 'Z') && ch != '_' && ch != '-') {
                    return ParseCode::InvalidMethod;
                }
                break;

            case State::SpacesBeforeUri:
                if (ch == '/') {
                    line_.uri_start = p;
                    state = State::AfterSlashInUri;
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'z') {
                    line_.schema_start = p;
                    state = State::Schema;
                    break;
                }
                if (ch == ' ') {
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::Schema:
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'z') {
                    break;
                }
                if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.') {
                    break;
                }
                if (ch == ':') {
                    line_.schema_end = p;
                    state = State::SchemaSlash;
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::SchemaSlash:
                if (ch == '/') {
                    state = State::SchemaSlashSlash;
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::SchemaSlashSlash:
                if (ch == '/') {
                    state = State::HostStart;
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::HostStart:
                line_.host_start = p;
                if (ch == '[') {
                    state = State::HostIpLiteral;
                    break;
                }
                state = State::Host;
                [[fallthrough]];

            case State::Host:
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'z') {
                    break;
                }
                if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
                    break;
                }
                state = State::HostEnd;
                [[fallthrough]];

            case State::HostEnd:
                line_.host_end = p;
                switch (ch) {
                    case ':':
                        state = State::Port;
                        break;
                    case '/':
                        line_.uri_start = p;
                        state = State::AfterSlashInUri;
                        break;
                    case '?':
                        line_.uri_start = p;
                        line_.args_start = p + 1;
                        line_.uri_parse.empty_path_in_uri = true;
                        line_.uri_parse.has_query = true;
                        line_.uri_parse.query_pos = 1;
                        state = State::Uri;
                        break;
                    case ' ':
                        if (line_.schema_end) {
                            line_.uri_start = line_.schema_end + 1;
                            line_.uri_end = line_.schema_end + 2;
                        } else {
                            line_.uri_start = p;
                            line_.uri_end = p;
                        }
                        state = State::Http09;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case State::HostIpLiteral:
                if ((ch >= '0' && ch <= '9')) {
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'z') {
                    break;
                }
                switch (ch) {
                    case ':':
                        break;
                    case ']':
                        state = State::HostEnd;
                        break;
                    case '-':
                    case '.':
                    case '_':
                    case '~':
                    case '!':
                    case '$':
                    case '&':
                    case '\'':
                    case '(':
                    case ')':
                    case '*':
                    case '+':
                    case ',':
                    case ';':
                    case '=':
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case State::Port:
                if (ch >= '0' && ch <= '9') {
                    break;
                }
                switch (ch) {
                    case '/':
                        line_.uri_start = p;
                        state = State::AfterSlashInUri;
                        break;
                    case '?':
                        line_.uri_start = p;
                        line_.args_start = p + 1;
                        line_.uri_parse.empty_path_in_uri = true;
                        line_.uri_parse.has_query = true;
                        line_.uri_parse.query_pos = 1;
                        state = State::Uri;
                        break;
                    case ' ':
                        if (line_.schema_end) {
                            line_.uri_start = line_.schema_end + 1;
                            line_.uri_end = line_.schema_end + 2;
                        } else {
                            line_.uri_start = p;
                            line_.uri_end = p;
                        }
                        state = State::Http09;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case State::AfterSlashInUri:
                if (detail::kUsual[ch >> 5] & (1U << (ch & 0x1f))) {
                    state = State::CheckUri;
                    break;
                }
                switch (ch) {
                    case ' ':
                        line_.uri_end = p;
                        state = State::Http09;
                        break;
                    case '\r':
                        line_.uri_end = p;
                        line_.http_minor = 9;
                        state = State::AlmostDone;
                        break;
                    case '\n':
                        line_.uri_end = p;
                        line_.http_minor = 9;
                        goto done;
                    case '.':
                        line_.uri_parse.complex_uri = true;
                        line_.uri_parse.has_exten = true;
                        line_.uri_parse.exten_pos = static_cast<std::size_t>((p + 1) - line_.uri_start);
                        state = State::Uri;
                        break;
                    case '%':
                        line_.uri_parse.quoted_uri = true;
                        state = State::Uri;
                        break;
                    case '/':
                        line_.uri_parse.complex_uri = true;
                        line_.uri_parse.has_exten = false;
                        line_.uri_parse.exten_pos = 0;
                        state = State::Uri;
                        break;
#if defined(_WIN32)
                    case '\\':
                        line_.uri_parse.complex_uri = true;
                        line_.uri_parse.has_exten = false;
                        line_.uri_parse.exten_pos = 0;
                        state = State::Uri;
                        break;
#endif
                    case '?':
                        line_.args_start = p + 1;
                        line_.uri_parse.has_query = true;
                        line_.uri_parse.query_pos = static_cast<std::size_t>((p + 1) - line_.uri_start);
                        state = State::Uri;
                        break;
                    case '#':
                        line_.uri_parse.complex_uri = true;
                        line_.uri_parse.has_fragment = true;
                        line_.uri_parse.fragment_pos = static_cast<std::size_t>(p - line_.uri_start);
                        state = State::Uri;
                        break;
                    case '+':
                        line_.uri_parse.plus_in_uri = true;
                        break;
                    default:
                        if (ch < 0x20 || ch == 0x7f) {
                            return ParseCode::InvalidRequest;
                        }
                        state = State::CheckUri;
                        break;
                }
                break;

            case State::CheckUri:
                if (detail::kUsual[ch >> 5] & (1U << (ch & 0x1f))) {
                    break;
                }
                switch (ch) {
                    case '/':
#if defined(_WIN32)
                        if (line_.uri_ext == p) {
                            line_.uri_parse.complex_uri = true;
                            state = State::Uri;
                            break;
                        }
#endif
                        line_.uri_ext = nullptr;
                        line_.uri_parse.has_exten = false;
                        line_.uri_parse.exten_pos = 0;
                        state = State::AfterSlashInUri;
                        break;
                    case '.':
                        line_.uri_ext = p + 1;
                        line_.uri_parse.has_exten = true;
                        line_.uri_parse.exten_pos = static_cast<std::size_t>((p + 1) - line_.uri_start);
                        break;
                    case ' ':
                        line_.uri_end = p;
                        state = State::Http09;
                        break;
                    case '\r':
                        line_.uri_end = p;
                        line_.http_minor = 9;
                        state = State::AlmostDone;
                        break;
                    case '\n':
                        line_.uri_end = p;
                        line_.http_minor = 9;
                        goto done;
#if defined(_WIN32)
                    case '\\':
                        line_.uri_parse.complex_uri = true;
                        line_.uri_parse.has_exten = false;
                        line_.uri_parse.exten_pos = 0;
                        state = State::AfterSlashInUri;
                        break;
#endif
                    case '%':
                        line_.uri_parse.quoted_uri = true;
                        state = State::Uri;
                        break;
                    case '?':
                        line_.args_start = p + 1;
                        line_.uri_parse.has_query = true;
                        line_.uri_parse.query_pos = static_cast<std::size_t>((p + 1) - line_.uri_start);
                        state = State::Uri;
                        break;
                    case '#':
                        line_.uri_parse.complex_uri = true;
                        line_.uri_parse.has_fragment = true;
                        line_.uri_parse.fragment_pos = static_cast<std::size_t>(p - line_.uri_start);
                        state = State::Uri;
                        break;
                    case '+':
                        line_.uri_parse.plus_in_uri = true;
                        break;
                    default:
                        if (ch < 0x20 || ch == 0x7f) {
                            return ParseCode::InvalidRequest;
                        }
                        break;
                }
                break;

            case State::Uri:
                if (detail::kUsual[ch >> 5] & (1U << (ch & 0x1f))) {
                    break;
                }
                switch (ch) {
                    case ' ':
                        line_.uri_end = p;
                        state = State::Http09;
                        break;
                    case '\r':
                        line_.uri_end = p;
                        line_.http_minor = 9;
                        state = State::AlmostDone;
                        break;
                    case '\n':
                        line_.uri_end = p;
                        line_.http_minor = 9;
                        goto done;
                    case '#':
                        line_.uri_parse.complex_uri = true;
                        if (!line_.uri_parse.has_fragment) {
                            line_.uri_parse.has_fragment = true;
                            line_.uri_parse.fragment_pos = static_cast<std::size_t>(p - line_.uri_start);
                        }
                        break;
                    case '?':
                        if (!line_.uri_parse.has_query) {
                            line_.uri_parse.has_query = true;
                            line_.uri_parse.query_pos = static_cast<std::size_t>((p + 1) - line_.uri_start);
                        }
                        break;
                    default:
                        if (ch < 0x20 || ch == 0x7f) {
                            return ParseCode::InvalidRequest;
                        }
                        break;
                }
                break;

            case State::Http09:
                switch (ch) {
                    case ' ':
                        break;
                    case '\r':
                        line_.http_minor = 9;
                        state = State::AlmostDone;
                        break;
                    case '\n':
                        line_.http_minor = 9;
                        goto done;
                    case 'H':
                        line_.http_protocol_start = p;
                        state = State::HttpH;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case State::HttpH:
                if (ch == 'T') {
                    state = State::HttpHT;
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::HttpHT:
                if (ch == 'T') {
                    state = State::HttpHTT;
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::HttpHTT:
                if (ch == 'P') {
                    state = State::HttpHTTP;
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::HttpHTTP:
                if (ch == '/') {
                    state = State::FirstMajorDigit;
                    break;
                }
                return ParseCode::InvalidRequest;

            case State::FirstMajorDigit:
                if (ch < '1' || ch > '9') {
                    return ParseCode::InvalidRequest;
                }
                line_.http_major = ch - '0';
                if (line_.http_major > 1) {
                    return ParseCode::InvalidVersion;
                }
                state = State::MajorDigit;
                break;

            case State::MajorDigit:
                if (ch == '.') {
                    state = State::FirstMinorDigit;
                    break;
                }
                if (ch < '0' || ch > '9') {
                    return ParseCode::InvalidRequest;
                }
                line_.http_major = line_.http_major * 10 + (ch - '0');
                if (line_.http_major > 1) {
                    return ParseCode::InvalidVersion;
                }
                break;

            case State::FirstMinorDigit:
                if (ch < '0' || ch > '9') {
                    return ParseCode::InvalidRequest;
                }
                line_.http_minor = ch - '0';
                state = State::MinorDigit;
                break;

            case State::MinorDigit:
                if (ch == '\r') {
                    state = State::AlmostDone;
                    break;
                }
                if (ch == '\n') {
                    goto done;
                }
                if (ch == ' ') {
                    state = State::SpacesAfterDigit;
                    break;
                }
                if (ch < '0' || ch > '9') {
                    return ParseCode::InvalidRequest;
                }
                if (line_.http_minor > 99) {
                    return ParseCode::InvalidRequest;
                }
                line_.http_minor = line_.http_minor * 10 + (ch - '0');
                break;

            case State::SpacesAfterDigit:
                switch (ch) {
                    case ' ':
                        break;
                    case '\r':
                        state = State::AlmostDone;
                        break;
                    case '\n':
                        goto done;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case State::AlmostDone:
                line_.request_end = p - 1;
                if (ch == '\n') {
                    goto done;
                }
                return ParseCode::InvalidRequest;
        }
    }

    buffer->consume(static_cast<std::size_t>(p - begin));
    state_ = state;
    return ParseCode::Again;

done:
    if (!line_.request_end) {
        std::uint8_t *buffer_start = buffer->data();
        if (p > buffer_start && *(p - 1) == '\r') {
            line_.request_end = (p > buffer_start + 1) ? (p - 2) : buffer_start;
        } else {
            line_.request_end = (p > buffer_start) ? (p - 1) : buffer_start;
        }
    }
    buffer->consume(static_cast<std::size_t>((p + 1) - begin));
    if (!line_.request_end) {
        line_.request_end = buffer->data();
    }
    line_.http_version = line_.http_major * 1000 + line_.http_minor;
    state_ = State::Start;
    if (line_.http_version == 9 && line_.method != HttpMethod::Get) {
        return ParseCode::Invalid09Method;
    }

    if (!line_.request_start || !line_.method_end || !line_.uri_start || !line_.uri_end) {
        return ParseCode::InvalidRequest;
    }

    return ParseCode::Ok;
}

void ResponseLineParser::reset() noexcept {
    state_ = State::Start;
    line_ = ResponseLineState{};
}

ParseCode ResponseLineParser::replace_buf_ptr(mem::IoBuf *old_buf, mem::IoBuf *new_buf) noexcept {
    FIBER_ASSERT(state_ != State::Start);
    FIBER_ASSERT(old_buf != nullptr);
    FIBER_ASSERT(new_buf != nullptr);
    FIBER_ASSERT(line_.line_start != nullptr);
    FIBER_ASSERT(old_buf->readable_data() > line_.line_start && old_buf->data() <= line_.line_start);
    std::uint8_t *new_buf_start = new_buf->writable_data();
    std::uint8_t *old = line_.line_start;
    auto length = static_cast<std::size_t>(old_buf->readable_data() - old);
    if (new_buf->writable() < length) {
        return ParseCode::HeaderTooLarge;
    }
    FIBER_ASSERT(length > 0);
    ::memcpy(new_buf_start, old, length);
    new_buf->commit(length);
    new_buf->consume(length);
    auto delta = new_buf_start - old;
    auto shift = [=](std::uint8_t *&ptr) {
        if (ptr) {
            ptr += delta;
        }
    };
    shift(line_.line_start);
    shift(line_.status_start);
    shift(line_.status_end);
    shift(line_.reason_start);
    shift(line_.reason_end);
    return ParseCode::Ok;
}

ParseCode ResponseLineParser::execute(mem::IoBuf *buffer) noexcept {
    if (!buffer || buffer->readable() == 0) {
        return ParseCode::Again;
    }

    auto *begin = buffer->readable_data();
    auto *p = begin;
    auto *end = buffer->writable_data();
    State state = state_;

    for (; p < end; ++p) {
        unsigned char ch = *p;
        switch (state) {
            case State::Start:
                line_.line_start = p;
                if (ch != 'H') {
                    return ParseCode::Error;
                }
                state = State::H;
                break;

            case State::H:
                if (ch != 'T') {
                    return ParseCode::Error;
                }
                state = State::HT;
                break;

            case State::HT:
                if (ch != 'T') {
                    return ParseCode::Error;
                }
                state = State::HTT;
                break;

            case State::HTT:
                if (ch != 'P') {
                    return ParseCode::Error;
                }
                state = State::HTTP;
                break;

            case State::HTTP:
                if (ch != '/') {
                    return ParseCode::Error;
                }
                state = State::FirstMajorDigit;
                break;

            case State::FirstMajorDigit:
                if (ch < '1' || ch > '9') {
                    return ParseCode::Error;
                }
                line_.http_major = ch - '0';
                state = State::MajorDigit;
                break;

            case State::MajorDigit:
                if (ch == '.') {
                    state = State::FirstMinorDigit;
                    break;
                }
                if (ch < '0' || ch > '9' || line_.http_major > 99) {
                    return ParseCode::Error;
                }
                line_.http_major = line_.http_major * 10 + (ch - '0');
                break;

            case State::FirstMinorDigit:
                if (ch < '0' || ch > '9') {
                    return ParseCode::Error;
                }
                line_.http_minor = ch - '0';
                state = State::MinorDigit;
                break;

            case State::MinorDigit:
                if (ch == ' ') {
                    state = State::Status;
                    break;
                }
                if (ch < '0' || ch > '9' || line_.http_minor > 99) {
                    return ParseCode::Error;
                }
                line_.http_minor = line_.http_minor * 10 + (ch - '0');
                break;

            case State::Status:
                if (ch == ' ') {
                    break;
                }
                if (ch < '0' || ch > '9') {
                    return ParseCode::Error;
                }
                if (line_.status_count == 0) {
                    line_.status_start = p;
                }
                line_.status_code = line_.status_code * 10 + (ch - '0');
                line_.status_end = p + 1;
                ++line_.status_count;
                if (line_.status_count == 3) {
                    state = State::SpaceAfterStatus;
                }
                break;

            case State::SpaceAfterStatus:
                switch (ch) {
                    case ' ':
                        line_.reason_start = p + 1;
                        line_.reason_end = nullptr;
                        state = State::StatusText;
                        break;
                    case '.':
                        line_.reason_start = p;
                        line_.reason_end = nullptr;
                        state = State::StatusText;
                        break;
                    case '\r':
                        line_.reason_start = nullptr;
                        line_.reason_end = nullptr;
                        state = State::AlmostDone;
                        break;
                    case '\n':
                        line_.reason_start = nullptr;
                        line_.reason_end = nullptr;
                        goto done;
                    default:
                        return ParseCode::Error;
                }
                break;

            case State::StatusText:
                switch (ch) {
                    case '\r':
                        line_.reason_end = p;
                        state = State::AlmostDone;
                        break;
                    case '\n':
                        line_.reason_end = p;
                        goto done;
                    default:
                        break;
                }
                break;

            case State::AlmostDone:
                if (ch != '\n') {
                    return ParseCode::Error;
                }
                goto done;
        }
    }

    buffer->consume(static_cast<std::size_t>(p - begin));
    state_ = state;
    return ParseCode::Again;

done:
    buffer->consume(static_cast<std::size_t>((p + 1) - begin));
    line_.http_version = line_.http_major * 1000 + line_.http_minor;
    if (line_.reason_start == nullptr) {
        line_.reason_end = nullptr;
    } else if (line_.reason_end == nullptr) {
        line_.reason_end = p;
    }
    state_ = State::Start;
    return ParseCode::Ok;
}

HeaderLineParser::HeaderLineParser() noexcept { reset(); }

HeaderLineParser::HeaderLineParser(const HttpServerOptions &) noexcept : HeaderLineParser() {}

void HeaderLineParser::reset() noexcept {
    line_ = HeaderLineState{};
    started_ = false;
    fiber_http1_header_line_init(&parser_);
    parser_.data = &line_;
}

ParseCode HeaderLineParser::replace_buf_ptr(mem::IoBuf *old_chain, mem::IoBuf *new_chain) noexcept {
    FIBER_ASSERT(started_);
    FIBER_ASSERT(old_chain->readable_data() > line_.header_name_start && old_chain->data() <= line_.header_name_start);
    std::uint8_t *new_buf_start = new_chain->writable_data();
    std::uint8_t *old = line_.header_name_start;
    auto length = static_cast<std::size_t>(old_chain->readable_data() - old);
    if (new_chain->writable() < length) {
        return ParseCode::HeaderTooLarge;
    }
    FIBER_ASSERT(length > 0);
    ::memcpy(new_buf_start, old, length);
    new_chain->commit(length);
    new_chain->consume(length);
    auto delta = new_buf_start - old;
    auto shift = [=](std::uint8_t *&ptr) {
        if (ptr) {
            ptr += delta;
        }
    };
    shift(line_.header_name_start);
    shift(line_.header_name_end);
    shift(line_.header_start);
    shift(line_.header_end);
    return ParseCode::Ok;
}

ParseCode HeaderLineParser::execute(mem::IoBuf *buffer) {
    if (!buffer || buffer->readable() == 0) {
        return ParseCode::Again;
    }

    auto *begin = buffer->readable_data();
    auto *end = buffer->writable_data();
    if (!started_) {
        line_ = HeaderLineState{};
        fiber_http1_header_line_init(&parser_);
        parser_.data = &line_;
        line_.header_name_start = begin;
        started_ = true;
    }

    int code = fiber_http1_header_line_execute(&parser_, reinterpret_cast<const char *>(begin),
                                               reinterpret_cast<const char *>(end));
    if (code == 0) {
        buffer->consume(static_cast<std::size_t>(end - begin));
        return ParseCode::Again;
    }

    if (code == FIBER_HTTP1_HEADER_LINE_INVALID) {
        auto *error_pos = reinterpret_cast<std::uint8_t *>(const_cast<char *>(parser_.error_pos));
        if (line_.header_end == nullptr || (error_pos < end && *error_pos == '\0')) {
            line_.header_end = error_pos;
        }
        return ParseCode::InvalidHeader;
    }

    FIBER_ASSERT(code == FIBER_HTTP1_HEADER_LINE_COMPLETE || code == FIBER_HTTP1_HEADER_LINE_HEADERS_COMPLETE);
    auto *line_after = reinterpret_cast<std::uint8_t *>(const_cast<char *>(parser_.error_pos));
    FIBER_ASSERT(begin < line_after && line_after <= end);
    buffer->consume(static_cast<std::size_t>(line_after - begin));
    started_ = false;
    fiber_http1_header_line_init(&parser_);
    parser_.data = &line_;
    return code == FIBER_HTTP1_HEADER_LINE_COMPLETE ? ParseCode::Ok : ParseCode::HeaderDone;
}

void ChunkedBodyParser::reset() noexcept {
    state_ = State::ChunkStart;
    size_ = 0;
    length_ = 0;
}

void ChunkedBodyParser::consume(std::size_t n) noexcept {
    if (n >= size_) {
        size_ = 0;
        return;
    }
    size_ -= n;
}

ParseCode ChunkedBodyParser::execute(mem::IoBuf *buffer) noexcept {
    if (!buffer) {
        return ParseCode::Error;
    }

    auto *begin = buffer->readable_data();
    auto *pos = begin;
    auto *end = buffer->writable_data();
    State state = state_;

    if (state == State::ChunkData && size_ == 0) {
        state = State::AfterData;
    }

    ParseCode rc = ParseCode::Again;
    constexpr std::size_t kMaxSize = std::numeric_limits<std::size_t>::max();

    auto append_hex_digit = [&](unsigned char digit) {
        if (size_ > (kMaxSize - digit) / 16) {
            return false;
        }
        size_ = size_ * 16 + digit;
        return true;
    };

    for (; pos < end; ++pos) {
        unsigned char ch = *pos;
        unsigned char c;

        switch (state) {
            case State::ChunkStart:
                if (ch >= '0' && ch <= '9') {
                    state = State::ChunkSize;
                    size_ = ch - '0';
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    state = State::ChunkSize;
                    size_ = c - 'a' + 10;
                    break;
                }
                goto invalid;

            case State::ChunkSize:
                if (ch >= '0' && ch <= '9') {
                    if (!append_hex_digit(static_cast<unsigned char>(ch - '0'))) {
                        goto invalid;
                    }
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    if (!append_hex_digit(static_cast<unsigned char>(c - 'a' + 10))) {
                        goto invalid;
                    }
                    break;
                }
                if (size_ == 0) {
                    switch (ch) {
                        case '\r':
                            state = State::LastChunkExtensionAlmostDone;
                            break;
                        case '\n':
                            state = State::Trailer;
                            rc = ParseCode::BodyDone;
                            ++pos;
                            goto data;
                        case ';':
                        case ' ':
                        case '\t':
                            state = State::LastChunkExtension;
                            break;
                        default:
                            goto invalid;
                    }
                    break;
                }
                switch (ch) {
                    case '\r':
                        state = State::ChunkExtensionAlmostDone;
                        break;
                    case '\n':
                        state = State::ChunkData;
                        break;
                    case ';':
                    case ' ':
                    case '\t':
                        state = State::ChunkExtension;
                        break;
                    default:
                        goto invalid;
                }
                break;

            case State::ChunkExtension:
                switch (ch) {
                    case '\r':
                        state = State::ChunkExtensionAlmostDone;
                        break;
                    case '\n':
                        state = State::ChunkData;
                        break;
                    default:
                        break;
                }
                break;

            case State::ChunkExtensionAlmostDone:
                if (ch == '\n') {
                    state = State::ChunkData;
                    break;
                }
                goto invalid;

            case State::ChunkData:
                rc = ParseCode::Ok;
                goto data;

            case State::AfterData:
                switch (ch) {
                    case '\r':
                        state = State::AfterDataAlmostDone;
                        break;
                    case '\n':
                        state = State::ChunkStart;
                        break;
                    default:
                        goto invalid;
                }
                break;

            case State::AfterDataAlmostDone:
                if (ch == '\n') {
                    state = State::ChunkStart;
                    break;
                }
                goto invalid;

            case State::LastChunkExtension:
                switch (ch) {
                    case '\r':
                        state = State::LastChunkExtensionAlmostDone;
                        break;
                    case '\n':
                        state = State::Trailer;
                        rc = ParseCode::BodyDone;
                        ++pos;
                        goto data;
                    default:
                        break;
                }
                break;

            case State::LastChunkExtensionAlmostDone:
                if (ch == '\n') {
                    state = State::Trailer;
                    rc = ParseCode::BodyDone;
                    ++pos;
                    goto data;
                }
                goto invalid;

            case State::Trailer:
            case State::TrailerAlmostDone:
            case State::TrailerHeader:
            case State::TrailerHeaderAlmostDone:
                rc = ParseCode::BodyDone;
                goto data;
        }
    }

data:
    state_ = state;
    buffer->consume(static_cast<std::size_t>(pos - begin));

    if (size_ > kMaxSize - 5) {
        return ParseCode::Error;
    }

    switch (state) {
        case State::ChunkStart:
            length_ = 3;
            break;
        case State::ChunkSize:
            length_ = 1 + (size_ ? size_ + 4 : 1);
            break;
        case State::ChunkExtension:
        case State::ChunkExtensionAlmostDone:
            length_ = 1 + size_ + 4;
            break;
        case State::ChunkData:
            length_ = size_ + 4;
            break;
        case State::AfterData:
        case State::AfterDataAlmostDone:
            length_ = 4;
            break;
        case State::LastChunkExtension:
        case State::LastChunkExtensionAlmostDone:
            length_ = 2;
            break;
        case State::Trailer:
        case State::TrailerAlmostDone:
        case State::TrailerHeader:
        case State::TrailerHeaderAlmostDone:
            length_ = 0;
            break;
    }

    return rc;

done:
    reset();
    buffer->consume(static_cast<std::size_t>((pos + 1) - begin));
    return ParseCode::Done;

invalid:
    return ParseCode::Error;
}

BodyParser::BodyParser() { reset(); }

void BodyParser::reset() { set_none(); }

void BodyParser::set_none() noexcept {
    type_ = Type::None;
    remaining_ = 0;
    done_ = true;
    chunked_parser_.reset();
}

void BodyParser::set_content_length(std::size_t length) noexcept {
    type_ = Type::ContentLength;
    remaining_ = length;
    done_ = length == 0;
    chunked_parser_.reset();
}

void BodyParser::set_chunked() noexcept {
    type_ = Type::Chunked;
    remaining_ = 0;
    done_ = false;
    chunked_parser_.reset();
}

std::size_t BodyParser::remaining() const noexcept {
    switch (type_) {
        case Type::None:
            return 0;
        case Type::ContentLength:
            return remaining_;
        case Type::Chunked:
            return chunked_parser_.size();
    }
    return 0;
}

void BodyParser::consume(std::size_t n) noexcept {
    switch (type_) {
        case Type::None:
            return;
        case Type::ContentLength:
            if (n >= remaining_) {
                remaining_ = 0;
                done_ = true;
                return;
            }
            remaining_ -= n;
            return;
        case Type::Chunked:
            chunked_parser_.consume(n);
            return;
    }
}

void BodyParser::finish_chunked_trailers() noexcept {
    if (type_ != Type::Chunked) {
        return;
    }
    done_ = true;
    chunked_parser_.reset();
}

ParseCode BodyParser::execute(mem::IoBuf *buffer) noexcept {
    switch (type_) {
        case Type::None:
            return ParseCode::Done;
        case Type::ContentLength:
            return done_ ? ParseCode::Done : ParseCode::Ok;
        case Type::Chunked: {
            if (done_) {
                return ParseCode::Done;
            }
            ParseCode code = chunked_parser_.execute(buffer);
            if (code == ParseCode::Done) {
                done_ = true;
            }
            return code;
        }
    }
    return ParseCode::Error;
}

} // namespace fiber::http

int fiber_http1_header_line_on_name(fiber_http1_header_line_t *parser, const unsigned char *p,
                                    const unsigned char *endp) {
    auto *line = static_cast<fiber::http::HeaderLineParser::HeaderLineState *>(parser->data);
    FIBER_ASSERT(line != nullptr);

    std::uint32_t hash = line->header_hash;
    std::uint32_t index = line->lowcase_index;
    for (; p < endp; ++p) {
        unsigned char lowercase = fiber::http::detail::kLowcase[*p];
        if (lowercase != 0) {
            hash = hash * 31 + lowercase;
            line->lowcase_header[index++] = lowercase;
            index &= fiber::http::HeaderLineParser::kLowcaseHeaderLen - 1;
        } else if (*p == '_') {
            hash = hash * 31 + *p;
            line->lowcase_header[index++] = *p;
            index &= fiber::http::HeaderLineParser::kLowcaseHeaderLen - 1;
        } else {
            line->invalid_header = true;
        }
    }
    line->header_hash = hash;
    line->lowcase_index = index;
    return 0;
}

int fiber_http1_header_line_on_value(fiber_http1_header_line_t *parser, const unsigned char *p,
                                     const unsigned char *endp) {
    auto *line = static_cast<fiber::http::HeaderLineParser::HeaderLineState *>(parser->data);
    FIBER_ASSERT(line != nullptr);

    if (line->header_start == nullptr) {
        line->header_start = reinterpret_cast<std::uint8_t *>(const_cast<unsigned char *>(p));
    }
    const unsigned char *value_end = endp;
    while (value_end > p && value_end[-1] == ' ') {
        --value_end;
    }
    if (value_end > p) {
        line->header_end = reinterpret_cast<std::uint8_t *>(const_cast<unsigned char *>(value_end));
    }
    return 0;
}

int fiber_http1_header_line_on_name_end(fiber_http1_header_line_t *parser, const unsigned char *p,
                                        const unsigned char *) {
    auto *line = static_cast<fiber::http::HeaderLineParser::HeaderLineState *>(parser->data);
    FIBER_ASSERT(line != nullptr);
    line->header_name_end = reinterpret_cast<std::uint8_t *>(const_cast<unsigned char *>(p));
    return 0;
}

int fiber_http1_header_line_on_empty_value(fiber_http1_header_line_t *parser, const unsigned char *p,
                                           const unsigned char *) {
    auto *line = static_cast<fiber::http::HeaderLineParser::HeaderLineState *>(parser->data);
    FIBER_ASSERT(line != nullptr);
    auto *position = reinterpret_cast<std::uint8_t *>(const_cast<unsigned char *>(p));
    line->header_start = position;
    line->header_end = position;
    return 0;
}

int fiber_http1_header_line_on_header_end(fiber_http1_header_line_t *parser, const unsigned char *p,
                                          const unsigned char *) {
    auto *line = static_cast<fiber::http::HeaderLineParser::HeaderLineState *>(parser->data);
    FIBER_ASSERT(line != nullptr);
    line->header_end = reinterpret_cast<std::uint8_t *>(const_cast<unsigned char *>(p));
    return 0;
}
