//
// Created by dear on 2026/7/7.
//

#include "JsValueDecode.h"

#include <cstdint>

namespace fiber::script::json {
namespace {

constexpr std::size_t kMaxDepth = 128;

enum class FrameKind : std::uint8_t {
    Array,
    Object,
};

struct Frame {
    FrameKind kind = FrameKind::Array;
    ValueHandle container;
    ValueHandle pending_key;
};

[[nodiscard]] bool decode_success(DecodeStatus status) noexcept {
    return status == DecodeStatus::Complete || status == DecodeStatus::Ok;
}

class DecodeBuilder final {
public:
    DecodeBuilder(GcHeap &heap, ValueHandle out) noexcept : heap_(heap), out_(out) {}

    [[nodiscard]] bool init() noexcept {
        if (!out_) {
            return set_error("invalid output handle");
        }
        *out_ = JsValue::make_undefined();
        scratch_ = heap_.local_value();
        if (!scratch_) {
            return set_error("out of memory");
        }
        return true;
    }

    [[nodiscard]] const char *error_message() const noexcept { return error_message_; }

    [[nodiscard]] fiber::json::DecodeCallbacks callbacks() noexcept {
        fiber::json::DecodeCallbacks callbacks;
        callbacks.ctx = this;
        callbacks.on_null = &DecodeBuilder::on_null;
        callbacks.on_bool = &DecodeBuilder::on_bool;
        callbacks.on_integer = &DecodeBuilder::on_integer;
        callbacks.on_double = &DecodeBuilder::on_double;
        callbacks.on_string = &DecodeBuilder::on_string;
        callbacks.on_object_key = &DecodeBuilder::on_object_key;
        callbacks.on_object_start = &DecodeBuilder::on_object_start;
        callbacks.on_object_end = &DecodeBuilder::on_object_end;
        callbacks.on_array_start = &DecodeBuilder::on_array_start;
        callbacks.on_array_end = &DecodeBuilder::on_array_end;
        return callbacks;
    }

private:
    [[nodiscard]] bool set_error(const char *message) noexcept {
        if (!error_message_) {
            error_message_ = message;
        }
        return false;
    }

    [[nodiscard]] bool attach_value(JsValue value) noexcept {
        if (!scratch_) {
            return set_error("invalid JSON builder state");
        }

        *scratch_ = value;
        if (depth_ == 0) {
            *out_ = *scratch_;
            *scratch_ = JsValue::make_undefined();
            return true;
        }

        Frame &frame = frames_[depth_ - 1];
        if (!frame.container) {
            return set_error("invalid JSON builder state");
        }

        if (frame.kind == FrameKind::Array) {
            if (js_value_type(*frame.container) != JsNodeType::Array) {
                return set_error("invalid JSON builder state");
            }
            if (!gc_array_push(&heap_, frame.container, *scratch_)) {
                return set_error("out of memory");
            }
            *scratch_ = JsValue::make_undefined();
            return true;
        }

        if (!frame.pending_key || js_value_is_undefined(*frame.pending_key)) {
            return set_error("object value without key");
        }
        if (js_value_type(*frame.container) != JsNodeType::Object ||
            js_value_type(*frame.pending_key) != JsNodeType::String) {
            return set_error("invalid JSON builder state");
        }
        if (!gc_object_set(&heap_, frame.container, *frame.pending_key, *scratch_)) {
            return set_error("out of memory");
        }
        *frame.pending_key = JsValue::make_undefined();
        *scratch_ = JsValue::make_undefined();
        return true;
    }

    [[nodiscard]] bool push_frame(FrameKind kind, JsValue container) noexcept {
        if (depth_ >= kMaxDepth) {
            return set_error("maximum JSON builder depth exceeded");
        }

        ValueHandle rooted_container = heap_.local_value();
        if (!rooted_container) {
            return set_error("out of memory");
        }
        *rooted_container = container;

        ValueHandle pending_key;
        if (kind == FrameKind::Object) {
            pending_key = heap_.local_value();
            if (!pending_key) {
                return set_error("out of memory");
            }
            *pending_key = JsValue::make_undefined();
        }

        frames_[depth_] = Frame{
                .kind = kind,
                .container = rooted_container,
                .pending_key = pending_key,
        };
        depth_ += 1;
        return true;
    }

    [[nodiscard]] bool pop_frame(FrameKind expected) noexcept {
        if (depth_ == 0) {
            return set_error("invalid JSON builder state");
        }

        Frame &frame = frames_[depth_ - 1];
        if (frame.kind != expected) {
            return set_error("invalid JSON builder state");
        }
        if (frame.pending_key && !js_value_is_undefined(*frame.pending_key)) {
            return set_error("object value without key");
        }

        if (frame.container) {
            *frame.container = JsValue::make_undefined();
        }
        if (frame.pending_key) {
            *frame.pending_key = JsValue::make_undefined();
        }
        frame = {};
        depth_ -= 1;
        return true;
    }

    [[nodiscard]] bool add_string(const char *data, std::size_t len) noexcept {
        if (!gc_make_string(&heap_, scratch_, data, len)) {
            return set_error("out of memory");
        }
        return attach_value(*scratch_);
    }

    [[nodiscard]] bool add_object_key(const char *data, std::size_t len) noexcept {
        if (depth_ == 0) {
            return set_error("invalid JSON builder state");
        }
        Frame &frame = frames_[depth_ - 1];
        if (frame.kind != FrameKind::Object || !frame.pending_key) {
            return set_error("invalid JSON builder state");
        }
        if (!js_value_is_undefined(*frame.pending_key)) {
            return set_error("invalid JSON builder state");
        }

        if (!gc_make_string(&heap_, frame.pending_key, data, len)) {
            return set_error("out of memory");
        }
        return true;
    }

    [[nodiscard]] bool start_array() noexcept {
        if (!gc_make_array(&heap_, scratch_, 0)) {
            return set_error("out of memory");
        }
        JsValue value = *scratch_;
        return attach_value(value) && push_frame(FrameKind::Array, value);
    }

    [[nodiscard]] bool start_object() noexcept {
        if (!gc_make_object(&heap_, scratch_, 0)) {
            return set_error("out of memory");
        }
        JsValue value = *scratch_;
        return attach_value(value) && push_frame(FrameKind::Object, value);
    }

    static int on_null(void *ctx) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->attach_value(JsValue::make_null()) ? 1 : 0;
    }

    static int on_bool(void *ctx, bool value) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->attach_value(JsValue::make_boolean(value)) ? 1 : 0;
    }

    static int on_integer(void *ctx, std::int64_t value) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->attach_value(JsValue::make_integer(value)) ? 1 : 0;
    }

    static int on_double(void *ctx, double value) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->attach_value(JsValue::make_float(value)) ? 1 : 0;
    }

    static int on_string(void *ctx, const char *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->add_string(data, len) ? 1 : 0;
    }

    static int on_object_key(void *ctx, const char *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->add_object_key(data, len) ? 1 : 0;
    }

    static int on_object_start(void *ctx) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->start_object() ? 1 : 0;
    }

    static int on_object_end(void *ctx) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->pop_frame(FrameKind::Object) ? 1 : 0;
    }

    static int on_array_start(void *ctx) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->start_array() ? 1 : 0;
    }

    static int on_array_end(void *ctx) noexcept {
        auto *self = static_cast<DecodeBuilder *>(ctx);
        return self && self->pop_frame(FrameKind::Array) ? 1 : 0;
    }

    GcHeap &heap_;
    ValueHandle out_;
    ValueHandle scratch_;
    Frame frames_[kMaxDepth] = {};
    std::size_t depth_ = 0;
    const char *error_message_ = nullptr;
};

} // namespace

DecodeStatus decode_js_value(GcHeap &heap, const char *data, std::size_t len, ValueHandle out,
                             ParseError *error) noexcept {
    if (error) {
        *error = {};
    }

    // Every object built during decoding stays rooted via out_/scratch_/frame
    // handles, and backing-store growth (array elems, object buckets/entries) is
    // freed eagerly, so a collection in/after the decode reclaims nothing. Wrap
    // the whole decode in one deferred NoGcScope: the per-primitive NoGcScopes
    // inside gc_make_*/gc_array_push/gc_object_set would otherwise return to
    // depth 0 after each primitive and fire a full collect() every time `bytes`
    // crosses the threshold. defer_collect drops the pending request at exit;
    // the next threshold-driven allocation re-evaluates naturally.
    GcHeap::NoGcScope no_gc(heap, /*defer_collect=*/true);
    GcHeap::LocalMark mark(heap);
    DecodeBuilder builder(heap, out);
    if (!builder.init()) {
        if (error) {
            error->message = builder.error_message();
            error->offset = 0;
        }
        return DecodeStatus::Error;
    }

    ParseError parse_error{};
    DecodeStatus status = fiber::json::decode(data, len, builder.callbacks(), &parse_error);
    if (!decode_success(status)) {
        if (out) {
            *out = JsValue::make_undefined();
        }
        if (status == DecodeStatus::Canceled && builder.error_message()) {
            parse_error.message = builder.error_message();
        }
    }

    if (error) {
        *error = parse_error;
    }
    return status;
}

DecodeStatus decode_js_value(GcHeap &heap, const char *data, std::size_t len, JsValue &out,
                             ParseError *error) noexcept {
    out = JsValue::make_undefined();

    GcHeap::LocalMark mark(heap);
    ValueHandle rooted = heap.local_value();
    if (!rooted) {
        if (error) {
            error->message = "out of memory";
            error->offset = 0;
        }
        return DecodeStatus::Error;
    }

    DecodeStatus status = decode_js_value(heap, data, len, rooted, error);
    if (decode_success(status)) {
        out = *rooted;
    }
    return status;
}

} // namespace fiber::script::json
