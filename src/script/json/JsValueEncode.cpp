//
// Created by dear on 2025/12/31.
//

#include <fiber/script/json/JsValueEncode.h>

#include <fiber/common/Assert.h>
#include <fiber/script/gc/GcInternal.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace fiber::script::json {
namespace {

// Longer strings and binaries go out in slices, so one value cannot hold back a flush. Escaping can
// grow a string slice sixfold. The binary slice is a whole number of 3-byte base64 groups.
constexpr std::size_t kStringSliceBytes = static_cast<std::size_t>(16 * 1024);
constexpr std::size_t kBinarySliceBytes = static_cast<std::size_t>(3 * 4096);

// End of the slice starting at pos, moved back onto a code point boundary. Text that is not valid
// UTF-8 has no boundary to find and is cut at the full slice length.
std::size_t string_slice_end(const char *data, std::size_t pos, std::size_t len) noexcept {
    const std::size_t end = pos + kStringSliceBytes;
    if (end >= len) {
        return len;
    }
    for (std::size_t cut = end; cut > end - 4; --cut) {
        if ((static_cast<unsigned char>(data[cut]) & 0xC0) != 0x80) {
            return cut;
        }
    }
    return end;
}

} // namespace

JsValueEncoder::JsValueEncoder(Generator &gen, const JsValue &root) noexcept : gen_(gen), root_(root) {}

JsValueEncoder::Status JsValueEncoder::run(std::size_t limit) noexcept {
    if (error_ != Generator::Result::OK) {
        return Status::Failed;
    }
    if (finished()) {
        return Status::Done;
    }
    const std::size_t written = gen_.bytes_written();
    stop_ = limit > std::numeric_limits<std::size_t>::max() - written ? std::numeric_limits<std::size_t>::max()
                                                                      : written + limit;
    for (;;) {
        const Generator::Result result = step();
        if (result != Generator::Result::OK) {
            error_ = result;
            return Status::Failed;
        }
        if (finished()) {
            return Status::Done;
        }
        if (gen_.bytes_written() >= stop_) {
            return Status::Yield;
        }
    }
}

Generator::Result JsValueEncoder::step() noexcept {
    if (token_ != Token::None) {
        return continue_token();
    }
    if (!started_) {
        started_ = true;
        return emit_value(root_);
    }
    Frame &top = frames_[depth_ - 1];
    switch (top.kind) {
        case Frame::Kind::Array:
            return step_array(top);
        case Frame::Kind::Object:
            return step_object(top);
        case Frame::Kind::Exception:
            return step_exception(top);
    }
    return Generator::Result::InvalidValue;
}

Generator::Result JsValueEncoder::step_array(Frame &frame) noexcept {
    const std::size_t depth = depth_;
    const GcArray *arr = frame.array;
    while (frame.index < arr->size) {
        const Generator::Result result = emit_value(arr->elems[frame.index++]);
        if (result != Generator::Result::OK || must_return(depth)) {
            return result;
        }
    }
    depth_ -= 1;
    return gen_.array_close();
}

Generator::Result JsValueEncoder::step_object(Frame &frame) noexcept {
    const std::size_t depth = depth_;
    const GcObject *obj = frame.object;
    while (frame.cursor != -1) {
        const GcObjectEntry &entry = obj->entries[frame.cursor];
        if (!frame.key_written) {
            if (!entry.occupied || !entry.key) {
                return Generator::Result::InvalidValue;
            }
            frame.key_written = true;
            const Generator::Result result = emit_gc_string(entry.key);
            if (result != Generator::Result::OK || must_return(depth)) {
                return result;
            }
        }
        frame.key_written = false;
        frame.cursor = entry.next_order;
        const Generator::Result result = emit_value(entry.value);
        if (result != Generator::Result::OK || must_return(depth)) {
            return result;
        }
    }
    depth_ -= 1;
    return gen_.map_close();
}

Generator::Result JsValueEncoder::step_exception(Frame &frame) noexcept {
    const GcException *exc = frame.exception;
    switch (frame.index++) {
        case 0:
            return gen_.string("position", 8);
        case 1:
            return gen_.integer(exc ? exc->position : -1);
        case 2:
            return gen_.string("name", 4);
        case 3: {
            if (exc) {
                return exc->name ? emit_gc_string(exc->name) : gen_.null_value();
            }
            const std::string_view name = exception_kind_name(frame.tagged);
            return gen_.string(name.data(), name.size());
        }
        case 4:
            return gen_.string("message", 7);
        case 5:
            return exc && exc->message ? emit_gc_string(exc->message) : gen_.null_value();
        case 6:
            return gen_.string("meta", 4);
        case 7:
            return exc ? emit_value(exc->meta) : gen_.null_value();
        default:
            depth_ -= 1;
            return gen_.map_close();
    }
}

Generator::Result JsValueEncoder::continue_token() noexcept {
    if (token_ == Token::String) {
        const std::size_t end = string_slice_end(token_data_, token_pos_, token_len_);
        const Generator::Result result = gen_.string_append(token_data_ + token_pos_, end - token_pos_);
        if (result != Generator::Result::OK) {
            return result;
        }
        token_pos_ = end;
        if (end != token_len_) {
            return Generator::Result::OK;
        }
        token_ = Token::None;
        return gen_.string_end();
    }
    const std::size_t end = std::min(token_pos_ + kBinarySliceBytes, token_len_);
    const Generator::Result result =
            gen_.binary_append(reinterpret_cast<const std::uint8_t *>(token_data_) + token_pos_, end - token_pos_);
    if (result != Generator::Result::OK) {
        return result;
    }
    token_pos_ = end;
    if (end != token_len_) {
        return Generator::Result::OK;
    }
    token_ = Token::None;
    return gen_.binary_end();
}

Generator::Result JsValueEncoder::emit_value(const JsValue &value) noexcept {
    switch (js_value_type(value)) {
        case JsNodeType::Null:
        case JsNodeType::Undefined:
            return gen_.null_value();
        case JsNodeType::Boolean:
            return gen_.bool_value(js_value_bool(value));
        case JsNodeType::Integer:
            return gen_.integer(js_value_int64(value));
        case JsNodeType::Float:
            return emit_double(js_value_double(value));
        case JsNodeType::String:
            if (js_value_is_borrowed_string(value)) {
                const NativeStr native = js_value_native_string(value);
                return emit_string(native.data, native.len);
            }
            return emit_gc_string(js_value_heap_ptr<const GcString>(value));
        case JsNodeType::Array: {
            const auto *arr = js_value_heap_ptr<const GcArray>(value);
            if (!arr) {
                return Generator::Result::InvalidValue;
            }
            const Generator::Result result = gen_.array_open();
            if (result != Generator::Result::OK) {
                return result;
            }
            push_frame(Frame::Kind::Array).array = arr;
            return Generator::Result::OK;
        }
        case JsNodeType::Object: {
            const auto *obj = js_value_heap_ptr<const GcObject>(value);
            if (!obj) {
                return Generator::Result::InvalidValue;
            }
            const Generator::Result result = gen_.map_open();
            if (result != Generator::Result::OK) {
                return result;
            }
            Frame &frame = push_frame(Frame::Kind::Object);
            frame.object = obj;
            frame.cursor = obj->head;
            return Generator::Result::OK;
        }
        case JsNodeType::Exception: {
            // Null for a tagged exception, which is a kind and nothing more.
            const auto *exc = js_value_heap_ptr<const GcException>(value);
            const Generator::Result result = gen_.map_open();
            if (result != Generator::Result::OK) {
                return result;
            }
            Frame &frame = push_frame(Frame::Kind::Exception);
            frame.exception = exc;
            if (!exc) {
                frame.tagged = js_value_exception_kind(value);
            }
            return Generator::Result::OK;
        }
        case JsNodeType::Binary:
            if (js_value_is_borrowed_binary(value)) {
                const NativeBin native = js_value_native_binary(value);
                return emit_binary(native.data, native.len);
            } else {
                const auto *bin = js_value_heap_ptr<const GcBinary>(value);
                if (!bin) {
                    return Generator::Result::InvalidString;
                }
                return emit_binary(bin->data, bin->len);
            }
        case JsNodeType::Interator:
            return Generator::Result::InvalidValue;
    }
    return Generator::Result::InvalidValue;
}

Generator::Result JsValueEncoder::emit_double(double value) noexcept {
    if (std::isnan(value)) {
        return gen_.string("NaN", 3);
    }
    if (std::isinf(value)) {
        return value < 0 ? gen_.string("-Infinity", 9) : gen_.string("Infinity", 8);
    }
    return gen_.double_value(value);
}

Generator::Result JsValueEncoder::emit_gc_string(const GcString *str) noexcept {
    std::string_view view;
    if (!gc_string_utf8_view(str, view)) {
        return Generator::Result::InvalidString;
    }
    return emit_string(view.data(), view.size());
}

Generator::Result JsValueEncoder::emit_string(const char *data, std::size_t len) noexcept {
    if (len <= kStringSliceBytes) {
        return gen_.string(data, len);
    }
    const Generator::Result result = gen_.string_begin();
    if (result != Generator::Result::OK) {
        return result;
    }
    token_ = Token::String;
    token_data_ = data;
    token_len_ = len;
    token_pos_ = 0;
    return Generator::Result::OK;
}

Generator::Result JsValueEncoder::emit_binary(const std::uint8_t *data, std::size_t len) noexcept {
    if (len <= kBinarySliceBytes) {
        return gen_.binary(data, len);
    }
    const Generator::Result result = gen_.binary_begin();
    if (result != Generator::Result::OK) {
        return result;
    }
    token_ = Token::Binary;
    token_data_ = reinterpret_cast<const char *>(data);
    token_len_ = len;
    token_pos_ = 0;
    return Generator::Result::OK;
}

JsValueEncoder::Frame &JsValueEncoder::push_frame(Frame::Kind kind) noexcept {
    FIBER_ASSERT(depth_ < Generator::MAX_STACK);
    Frame &frame = frames_[depth_];
    depth_ += 1;
    frame.index = 0;
    frame.cursor = -1;
    frame.kind = kind;
    frame.key_written = false;
    return frame;
}

Generator::Result encode_js_value(Generator &gen, const JsValue &value) {
    JsValueEncoder encoder(gen, value);
    return encoder.run(std::numeric_limits<std::size_t>::max()) == JsValueEncoder::Status::Done ? Generator::Result::OK
                                                                                                : encoder.error();
}

} // namespace fiber::script::json
