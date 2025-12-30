//
// Created by dear on 2025/12/29.
//

#ifndef FIBER_JSONENCODE_H
#define FIBER_JSONENCODE_H
#include <cstddef>
#include <cstdint>
#include <string>

namespace fiber::json {
class OutputSink {
public:
    virtual ~OutputSink() = default;
    [[nodiscard]] virtual bool write(const char *data, size_t len) = 0;
    virtual void reset() {}
};

class CallbackSink final : public OutputSink {
public:
    using PrintCallback = int (*)(void *ctx, const char *str, size_t len);

    CallbackSink(PrintCallback cb, void *ctx);
    [[nodiscard]] bool write(const char *data, size_t len) override;

private:
    PrintCallback callback_;
    void *ctx_;
};

class Generator {
public:
    enum class State { Start, MapStart, MapKey, MapValue, ArrayStart, InArray, Complete, Error };
    enum class Result {
        /** no error */
        OK = 0,
        /** at a point where a map key is generated, a function other than
         *  yajl_gen_string was called */
        KeysMustBeString,
        /** YAJL's maximum generation depth was exceeded.  see
         *  YAJL_MAX_DEPTH */
        MaxDepthExceeded,
        /** A generator function (yajl_gen_XXX) was called while in an error
         *  state */
        ErrorState,
        /** A complete JSON document has been generated */
        GenerateComplete,
        /** yajl_gen_double was passed an invalid floating point value
         *  (infinity or NaN). */
        InvalidValue,
        /** returned from yajl_gen_string() when the yajl_gen_validate_utf8
         *  option is enabled and an invalid was passed by client code.
         */
        InvalidString
    };

    enum class Option : uint32_t {
        /** generate indented (beautiful) output */
        Beauty = 0x01,
        /**
         * Set an indent string which is used when yajl_gen_beautify
         * is enabled.  Maybe something like \\t or some number of
         * spaces.  The default is four spaces ' '.
         */
        IndentString = 0x02,
        /**
         * Normally the generator does not validate that strings you
         * pass to it via yajl_gen_string() are valid UTF8.  Enabling
         * this option will cause it to do so.
         */
        ValidateUtf8 = 0x08,
        /**
         * the forward solidus (slash or '/' in human) is not required to be
         * escaped in json text.  By default, YAJL will not escape it in the
         * iterest of saving bytes.  Setting this flag will cause YAJL to
         * always escape '/' in generated JSON strings.
         */
        EscapeSolidus = 0x10
    };

    explicit Generator(OutputSink &sink);
    Generator(const Generator &) = delete;
    Generator &operator=(const Generator &) = delete;
    Generator(Generator &&) = delete;
    Generator &operator=(Generator &&) = delete;

    void clear();
    void set_option(Option opt, bool enabled = true);
    void set_indent_string(const std::string &indent);

    [[nodiscard]] State get_state() const;

    Result map_open();
    Result map_close();
    Result array_open();
    Result array_close();

    Result string(const char *str, size_t len);
    Result string(const std::string &str);
    Result integer(int64_t value);
    Result double_value(double value);
    Result bool_value(bool value);
    Result null_value();


private:
    static constexpr size_t MAX_STACK = 128;

    [[nodiscard]] bool has_option(Option opt) const;
    [[nodiscard]] Result append(const char *data, size_t len);
    [[nodiscard]] Result append(char ch);
    [[nodiscard]] Result append_indent(size_t level);
    [[nodiscard]] Result prefix_for_value();
    [[nodiscard]] Result prefix_for_key();
    [[nodiscard]] Result write_string(const char *str, size_t len);
    [[nodiscard]] Result finish_value();
    [[nodiscard]] Result push(State state);
    [[nodiscard]] Result pop();
    [[nodiscard]] Result set_error(Result result);
    [[nodiscard]] State &current_state();
    [[nodiscard]] const State &current_state() const;

    OutputSink &sink_;
    State state_stack_[MAX_STACK] = {};
    size_t depth_ = 1;
    uint32_t options_ = 0;
    std::string indent_string_ = "    ";
};

} // namespace fiber::json
#endif // FIBER_JSONENCODE_H
