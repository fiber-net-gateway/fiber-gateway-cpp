//
// Created by dear on 2025/12/29.
//

#ifndef FIBER_JSONENCODE_H
#define FIBER_JSONENCODE_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::json {

class OutputSink {
public:
    virtual ~OutputSink() = default;
    [[nodiscard]] virtual bool write(const char *data, size_t len) noexcept = 0;
    virtual void reset() noexcept {}
};

class CallbackSink final : public OutputSink {
public:
    using PrintCallback = int (*)(void *ctx, const char *str, size_t len) noexcept;

    CallbackSink(PrintCallback cb, void *ctx) noexcept;
    [[nodiscard]] bool write(const char *data, size_t len) noexcept override;

private:
    PrintCallback callback_;
    void *ctx_;
};

// Appends to a caller-owned std::string. An allocation failure fails the write, so the
// generator stops with ErrorState; out_of_memory() tells that apart from an encoding error.
class StringSink final : public OutputSink {
public:
    explicit StringSink(std::string &out) noexcept : out_(out) {}
    [[nodiscard]] bool write(const char *data, size_t len) noexcept override;
    [[nodiscard]] bool out_of_memory() const noexcept { return out_of_memory_; }

private:
    std::string &out_;
    bool out_of_memory_ = false;
};

class Generator {
public:
    // Nesting bound: at most MAX_STACK - 1 containers can be open at once.
    static constexpr size_t MAX_STACK = 128;

    enum class State {
        Start,
        MapStart,
        MapKey,
        MapValue,
        ArrayStart,
        InArray,
        Complete,
        Error,
        // Between string_begin() and string_end().
        InKeyString,
        InValueString,
        // Between binary_begin() and binary_end(); InBinaryTail once a piece has closed the base64 groups.
        InBinary,
        InBinaryTail
    };
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

    explicit Generator(OutputSink &sink) noexcept;
    Generator(const Generator &) = delete;
    Generator &operator=(const Generator &) = delete;
    Generator(Generator &&) = delete;
    Generator &operator=(Generator &&) = delete;

    void clear() noexcept;
    void set_option(Option opt, bool enabled = true) noexcept;
    // Returns false, keeping the current indent, when the copy cannot be allocated.
    [[nodiscard]] bool set_indent_string(std::string_view indent) noexcept;

    [[nodiscard]] State get_state() const noexcept;
    // Total bytes given to the sink since construction or clear().
    [[nodiscard]] size_t bytes_written() const noexcept { return bytes_written_; }

    Result map_open() noexcept;
    Result map_close() noexcept;
    Result array_open() noexcept;
    Result array_close() noexcept;

    Result string(const char *str, size_t len) noexcept;
    Result string(const std::string &str) noexcept;
    // One string written in pieces, so a caller can stop between them. Works as a value or a map
    // key, like string(). Anything else called before string_end() is an error. With ValidateUtf8
    // each piece is validated alone, so pieces must split on code point boundaries.
    Result string_begin() noexcept;
    Result string_append(const char *str, size_t len) noexcept;
    Result string_end() noexcept;
    // Binary values are encoded as base64 JSON strings.
    Result binary(const std::uint8_t *data, size_t len) noexcept;
    // One binary value written in pieces. Every piece but the last must be a multiple of 3 bytes,
    // because a shorter group is padded; appending after such a piece is an error.
    Result binary_begin() noexcept;
    Result binary_append(const std::uint8_t *data, size_t len) noexcept;
    Result binary_end() noexcept;
    Result integer(int64_t value) noexcept;
    Result double_value(double value) noexcept;
    Result bool_value(bool value) noexcept;
    Result null_value() noexcept;


private:
    [[nodiscard]] bool has_option(Option opt) const noexcept;
    [[nodiscard]] Result append(const char *data, size_t len) noexcept;
    [[nodiscard]] Result append(char ch) noexcept;
    [[nodiscard]] Result append_indent(size_t level) noexcept;
    [[nodiscard]] Result prefix_for_value() noexcept;
    [[nodiscard]] Result prefix_for_key() noexcept;
    [[nodiscard]] Result write_string(const char *str, size_t len) noexcept;
    [[nodiscard]] Result write_string_content(const char *str, size_t len) noexcept;
    [[nodiscard]] Result write_escape(unsigned char ch) noexcept;
    [[nodiscard]] Result finish_key() noexcept;
    [[nodiscard]] Result finish_value() noexcept;
    [[nodiscard]] Result push(State state) noexcept;
    [[nodiscard]] Result pop() noexcept;
    void enter_piece(State state) noexcept;
    [[nodiscard]] Result set_error(Result result) noexcept;
    [[nodiscard]] State &current_state() noexcept;
    [[nodiscard]] const State &current_state() const noexcept;

    OutputSink &sink_;
    // One slot past the container bound, for the piece state of an open string or binary.
    State state_stack_[MAX_STACK + 1] = {};
    size_t depth_ = 1;
    size_t bytes_written_ = 0;
    uint32_t options_ = 0;
    std::string indent_string_ = "    ";
};

} // namespace fiber::json
#endif // FIBER_JSONENCODE_H
