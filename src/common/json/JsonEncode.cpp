#include "JsonEncode.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace fiber::json {
    namespace {

        bool is_valid_utf8(const unsigned char *data, size_t len) {
            size_t i = 0;
            while (i < len) {
                unsigned char c = data[i];
                if (c < 0x80) {
                    i += 1;
                    continue;
                }
                if ((c & 0xE0) == 0xC0) {
                    if (i + 1 >= len) {
                        return false;
                    }
                    unsigned char c1 = data[i + 1];
                    if ((c1 & 0xC0) != 0x80) {
                        return false;
                    }
                    uint32_t code = ((c & 0x1F) << 6) | (c1 & 0x3F);
                    if (code < 0x80) {
                        return false;
                    }
                    i += 2;
                    continue;
                }
                if ((c & 0xF0) == 0xE0) {
                    if (i + 2 >= len) {
                        return false;
                    }
                    unsigned char c1 = data[i + 1];
                    unsigned char c2 = data[i + 2];
                    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
                        return false;
                    }
                    uint32_t code = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
                    if (code < 0x800) {
                        return false;
                    }
                    if (code >= 0xD800 && code <= 0xDFFF) {
                        return false;
                    }
                    i += 3;
                    continue;
                }
                if ((c & 0xF8) == 0xF0) {
                    if (i + 3 >= len) {
                        return false;
                    }
                    unsigned char c1 = data[i + 1];
                    unsigned char c2 = data[i + 2];
                    unsigned char c3 = data[i + 3];
                    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
                        return false;
                    }
                    uint32_t code = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
                    if (code < 0x10000 || code > 0x10FFFF) {
                        return false;
                    }
                    i += 4;
                    continue;
                }
                return false;
            }
            return true;
        }

    } // namespace

    Generator::Generator() { clear(); }

    void Generator::clear() {
        buffer_.clear();
        depth_ = 1;
        state_stack_[0] = State::Start;
    }

    void Generator::set_option(Option opt, bool enabled) {
        uint32_t bit = static_cast<uint32_t>(opt);
        if (enabled) {
            options_ |= bit;
        } else {
            options_ &= ~bit;
        }
        if (opt == Option::PrintCallback && !enabled) {
            print_callback_ = nullptr;
            print_callback_ctx_ = nullptr;
        }
    }

    void Generator::set_indent_string(const std::string &indent) {
        indent_string_ = indent;
        set_option(Option::IndentString, true);
    }

    void Generator::set_print_callback(PrintCallback cb, void *ctx) {
        print_callback_ = cb;
        print_callback_ctx_ = ctx;
        set_option(Option::PrintCallback, cb != nullptr);
    }

    Generator::State Generator::get_state() const { return current_state(); }

    Generator::Result Generator::get_buf(const unsigned char **buf, size_t *len) const {
        if (print_callback_ != nullptr) {
            return Result::NoBuf;
        }
        if (buf) {
            *buf = reinterpret_cast<const unsigned char *>(buffer_.data());
        }
        if (len) {
            *len = buffer_.size();
        }
        return Result::OK;
    }

    Generator::Result Generator::map_open() {
        Result result = prefix_for_value();
        if (result != Result::OK) {
            return result;
        }
        result = append('{');
        if (result != Result::OK) {
            return result;
        }
        return push(State::MapStart);
    }

    Generator::Result Generator::map_close() {
        State state = current_state();
        if (state != State::MapStart && state != State::MapKey) {
            return set_error(Result::ErrorState);
        }
        if (has_option(Option::Beauty) && state == State::MapKey && depth_ >= 2) {
            Result result = append('\n');
            if (result != Result::OK) {
                return result;
            }
            result = append_indent(depth_ - 2);
            if (result != Result::OK) {
                return result;
            }
        }
        Result result = append('}');
        if (result != Result::OK) {
            return result;
        }
        result = pop();
        if (result != Result::OK) {
            return result;
        }
        return finish_value();
    }

    Generator::Result Generator::array_open() {
        Result result = prefix_for_value();
        if (result != Result::OK) {
            return result;
        }
        result = append('[');
        if (result != Result::OK) {
            return result;
        }
        return push(State::ArrayStart);
    }

    Generator::Result Generator::array_close() {
        State state = current_state();
        if (state != State::ArrayStart && state != State::InArray) {
            return set_error(Result::ErrorState);
        }
        if (has_option(Option::Beauty) && state == State::InArray && depth_ >= 2) {
            Result result = append('\n');
            if (result != Result::OK) {
                return result;
            }
            result = append_indent(depth_ - 2);
            if (result != Result::OK) {
                return result;
            }
        }
        Result result = append(']');
        if (result != Result::OK) {
            return result;
        }
        result = pop();
        if (result != Result::OK) {
            return result;
        }
        return finish_value();
    }

    Generator::Result Generator::string(const char *str, size_t len) {
        State state = current_state();
        if (state == State::MapStart || state == State::MapKey) {
            Result result = prefix_for_key();
            if (result != Result::OK) {
                return result;
            }
            result = write_string(str, len);
            if (result != Result::OK) {
                return result;
            }
            if (has_option(Option::Beauty)) {
                result = append(": ", 2);
            } else {
                result = append(':');
            }
            if (result != Result::OK) {
                return result;
            }
            current_state() = State::MapValue;
            return Result::OK;
        }
        Result result = prefix_for_value();
        if (result != Result::OK) {
            return result;
        }
        result = write_string(str, len);
        if (result != Result::OK) {
            return result;
        }
        return finish_value();
    }

    Generator::Result Generator::string(const std::string &str) { return string(str.data(), str.size()); }

    Generator::Result Generator::integer(int64_t value) {
        Result result = prefix_for_value();
        if (result != Result::OK) {
            return result;
        }
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
        if (ec != std::errc()) {
            return set_error(Result::InvalidValue);
        }
        result = append(buf, static_cast<size_t>(ptr - buf));
        if (result != Result::OK) {
            return result;
        }
        return finish_value();
    }

    Generator::Result Generator::double_value(double value) {
        if (!std::isfinite(value)) {
            return set_error(Result::InvalidValue);
        }
        Result result = prefix_for_value();
        if (result != Result::OK) {
            return result;
        }
        char buf[64];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general,
                                       std::numeric_limits<double>::max_digits10);
        if (ec != std::errc()) {
            return set_error(Result::InvalidValue);
        }
        result = append(buf, static_cast<size_t>(ptr - buf));
        if (result != Result::OK) {
            return result;
        }
        return finish_value();
    }

    Generator::Result Generator::bool_value(bool value) {
        Result result = prefix_for_value();
        if (result != Result::OK) {
            return result;
        }
        if (value) {
            result = append("true", 4);
        } else {
            result = append("false", 5);
        }
        if (result != Result::OK) {
            return result;
        }
        return finish_value();
    }

    Generator::Result Generator::null_value() {
        Result result = prefix_for_value();
        if (result != Result::OK) {
            return result;
        }
        result = append("null", 4);
        if (result != Result::OK) {
            return result;
        }
        return finish_value();
    }

    bool Generator::has_option(Option opt) const { return (options_ & static_cast<uint32_t>(opt)) != 0; }

    Generator::Result Generator::append(const char *data, size_t len) {
        if (len == 0) {
            return Result::OK;
        }
        if (!data) {
            return set_error(Result::ErrorState);
        }
        if (print_callback_ != nullptr) {
            int rc = print_callback_(print_callback_ctx_, data, len);
            if (rc != 0) {
                return set_error(Result::ErrorState);
            }
            return Result::OK;
        }
        if (!buffer_.append(data, len)) {
            return set_error(Result::ErrorState);
        }
        return Result::OK;
    }

    Generator::Result Generator::append(char ch) { return append(&ch, 1); }

    Generator::Result Generator::append_indent(size_t level) {
        if (indent_string_.empty()) {
            return Result::OK;
        }
        for (size_t i = 0; i < level; ++i) {
            Result result = append(indent_string_.data(), indent_string_.size());
            if (result != Result::OK) {
                return result;
            }
        }
        return Result::OK;
    }

    Generator::Result Generator::prefix_for_value() {
        State state = current_state();
        if (state == State::Error) {
            return Result::ErrorState;
        }
        if (state == State::Complete) {
            return Result::GenerateComplete;
        }
        if (state == State::MapStart || state == State::MapKey) {
            return set_error(Result::KeysMustBeString);
        }
        if (state == State::ArrayStart) {
            if (has_option(Option::Beauty)) {
                Result result = append('\n');
                if (result != Result::OK) {
                    return result;
                }
                return append_indent(depth_ - 1);
            }
        } else if (state == State::InArray) {
            Result result = append(',');
            if (result != Result::OK) {
                return result;
            }
            if (has_option(Option::Beauty)) {
                result = append('\n');
                if (result != Result::OK) {
                    return result;
                }
                return append_indent(depth_ - 1);
            }
        }
        return Result::OK;
    }

    Generator::Result Generator::prefix_for_key() {
        State state = current_state();
        if (state == State::Error) {
            return Result::ErrorState;
        }
        if (state == State::Complete) {
            return Result::GenerateComplete;
        }
        if (state == State::MapStart) {
            if (has_option(Option::Beauty)) {
                Result result = append('\n');
                if (result != Result::OK) {
                    return result;
                }
                return append_indent(depth_ - 1);
            }
            return Result::OK;
        }
        if (state == State::MapKey) {
            Result result = append(',');
            if (result != Result::OK) {
                return result;
            }
            if (has_option(Option::Beauty)) {
                result = append('\n');
                if (result != Result::OK) {
                    return result;
                }
                return append_indent(depth_ - 1);
            }
            return Result::OK;
        }
        return set_error(Result::ErrorState);
    }

    Generator::Result Generator::write_string(const char *str, size_t len) {
        if (!str && len > 0) {
            return set_error(Result::InvalidString);
        }
        if (has_option(Option::ValidateUtf8)) {
            if (!is_valid_utf8(reinterpret_cast<const unsigned char *>(str), len)) {
                return set_error(Result::InvalidString);
            }
        }
        Result result = append('\"');
        if (result != Result::OK) {
            return result;
        }
        const unsigned char *data = reinterpret_cast<const unsigned char *>(str);
        for (size_t i = 0; i < len; ++i) {
            unsigned char ch = data[i];
            switch (ch) {
                case '\"':
                    result = append("\\\"", 2);
                    break;
                case '\\':
                    result = append("\\\\", 2);
                    break;
                case '\b':
                    result = append("\\b", 2);
                    break;
                case '\f':
                    result = append("\\f", 2);
                    break;
                case '\n':
                    result = append("\\n", 2);
                    break;
                case '\r':
                    result = append("\\r", 2);
                    break;
                case '\t':
                    result = append("\\t", 2);
                    break;
                case '/':
                    if (has_option(Option::EscapeSolidus)) {
                        result = append("\\/", 2);
                    } else {
                        result = append('/');
                    }
                    break;
                default:
                    if (ch < 0x20) {
                        char buf[6] = {'\\', 'u', '0', '0', '0', '0'};
                        const char *hex = "0123456789ABCDEF";
                        buf[4] = hex[(ch >> 4) & 0x0F];
                        buf[5] = hex[ch & 0x0F];
                        result = append(buf, sizeof(buf));
                    } else {
                        result = append(static_cast<char>(ch));
                    }
                    break;
            }
            if (result != Result::OK) {
                return result;
            }
        }
        return append('\"');
    }

    Generator::Result Generator::finish_value() {
        State &state = current_state();
        switch (state) {
            case State::Start:
                state = State::Complete;
                return Result::OK;
            case State::MapValue:
                state = State::MapKey;
                return Result::OK;
            case State::ArrayStart:
            case State::InArray:
                state = State::InArray;
                return Result::OK;
            case State::Complete:
                return Result::GenerateComplete;
            case State::Error:
                return Result::ErrorState;
            case State::MapStart:
            case State::MapKey:
                return set_error(Result::KeysMustBeString);
        }
        return set_error(Result::ErrorState);
    }

    Generator::Result Generator::push(State state) {
        if (depth_ >= MAX_STACK) {
            return set_error(Result::MaxDepthExceeded);
        }
        state_stack_[depth_] = state;
        depth_ += 1;
        return Result::OK;
    }

    Generator::Result Generator::pop() {
        if (depth_ <= 1) {
            return set_error(Result::ErrorState);
        }
        depth_ -= 1;
        return Result::OK;
    }

    Generator::Result Generator::set_error(Result result) {
        current_state() = State::Error;
        return result;
    }

    Generator::State &Generator::current_state() { return state_stack_[depth_ - 1]; }

    const Generator::State &Generator::current_state() const { return state_stack_[depth_ - 1]; }

} // namespace fiber::json
