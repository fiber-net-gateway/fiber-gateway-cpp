//
// Created by dear on 2025/12/31.
//

#ifndef FIBER_JSVALUEENCODE_H
#define FIBER_JSVALUEENCODE_H

#include <cstddef>
#include <cstdint>

#include "../../common/json/JsonEncode.h"
#include "../JsValue.h"

namespace fiber::script {
struct GcArray;
struct GcException;
struct GcObject;
struct GcString;
} // namespace fiber::script

namespace fiber::script::json {

using fiber::json::Generator;

// Encodes a value in steps, so a caller can send what the sink holds and carry on.
//
// Values encode as JSON text, with three additions: undefined is null, Binary is a base64 string,
// and a non-finite number is the string "NaN", "Infinity" or "-Infinity".
//
// The encoder keeps raw pointers into the GC heap between run() calls, so nothing may mutate or
// collect that heap until it has finished.
class JsValueEncoder {
public:
    enum class Status : std::uint8_t {
        Done, // The whole value is written.
        Yield, // The limit was reached; call run() again to continue.
        Failed, // See error(). Calling run() again returns Failed.
    };

    JsValueEncoder(Generator &gen, const JsValue &root) noexcept;
    JsValueEncoder(const JsValueEncoder &) = delete;
    JsValueEncoder &operator=(const JsValueEncoder &) = delete;
    JsValueEncoder(JsValueEncoder &&) = delete;
    JsValueEncoder &operator=(JsValueEncoder &&) = delete;

    // Writes until limit more bytes reached the generator or the value ended. Always makes
    // progress, and stops only between tokens or between the slices of a long string or binary, so
    // the output can pass limit by one slice: 96 KiB at most, if every byte of it needs escaping.
    [[nodiscard]] Status run(std::size_t limit) noexcept;
    [[nodiscard]] Generator::Result error() const noexcept { return error_; }

private:
    struct Frame {
        enum class Kind : std::uint8_t { Array, Object, Exception };

        union {
            const GcArray *array;
            const GcObject *object;
            const GcException *exception;
        };
        std::size_t index; // Array: next element. Exception: next field step.
        std::int32_t cursor; // Object: next entry, -1 at the end.
        Kind kind;
        bool key_written; // Object: the entry at cursor has its key out, its value is next.
        ExceptionKind tagged; // Exception: what it is when exception is null, as a tagged one has no details.
    };

    enum class Token : std::uint8_t { None, String, Binary };

    [[nodiscard]] bool finished() const noexcept { return started_ && depth_ == 0 && token_ == Token::None; }
    // Whether the frame at depth has to hand back to run(): a child frame or a sliced token is next,
    // or the limit is reached. Frames otherwise loop over their scalars without leaving.
    [[nodiscard]] bool must_return(std::size_t depth) const noexcept {
        return depth_ != depth || token_ != Token::None || gen_.bytes_written() >= stop_;
    }
    [[nodiscard]] Generator::Result step() noexcept;
    [[nodiscard]] Generator::Result step_array(Frame &frame) noexcept;
    [[nodiscard]] Generator::Result step_object(Frame &frame) noexcept;
    [[nodiscard]] Generator::Result step_exception(Frame &frame) noexcept;
    [[nodiscard]] Generator::Result continue_token() noexcept;
    [[nodiscard]] Generator::Result emit_value(const JsValue &value) noexcept;
    [[nodiscard]] Generator::Result emit_double(double value) noexcept;
    [[nodiscard]] Generator::Result emit_gc_string(const GcString *str) noexcept;
    [[nodiscard]] Generator::Result emit_string(const char *data, std::size_t len) noexcept;
    [[nodiscard]] Generator::Result emit_binary(const std::uint8_t *data, std::size_t len) noexcept;
    [[nodiscard]] Frame &push_frame(Frame::Kind kind) noexcept;

    Generator &gen_;
    JsValue root_;
    Generator::Result error_ = Generator::Result::OK;
    bool started_ = false;
    Token token_ = Token::None;
    const char *token_data_ = nullptr;
    std::size_t token_len_ = 0;
    std::size_t token_pos_ = 0;
    std::size_t depth_ = 0;
    std::size_t stop_ = 0;
    // The generator refuses to open more containers than this, so the stack cannot overflow.
    Frame frames_[Generator::MAX_STACK];
};

Generator::Result encode_js_value(Generator &gen, const JsValue &value);

} // namespace fiber::script::json

#endif // FIBER_JSVALUEENCODE_H
