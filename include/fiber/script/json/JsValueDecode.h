//
// Created by dear on 2026/7/7.
//

#ifndef FIBER_JSVALUEDECODE_H
#define FIBER_JSVALUEDECODE_H

#include <cstddef>
#include <memory>

#include "../../common/json/JsonDecode.h"
#include "../JsGc.h"

namespace fiber::script::json {

using fiber::json::DecodeStatus;
using fiber::json::ParseError;

[[nodiscard]] DecodeStatus decode_js_value(GcHeap &heap, const char *data, std::size_t len, ValueHandle out,
                                           ParseError *error = nullptr) noexcept;

[[nodiscard]] DecodeStatus decode_js_value(GcHeap &heap, const char *data, std::size_t len, JsValue &out,
                                           ParseError *error = nullptr) noexcept;

// Builds a value from JSON arriving in pieces, so the text never has to exist in one place.
// The caller holds a GcHeap::LocalMark around the whole decode and keeps out rooted; both may
// span suspensions, because everything built so far stays rooted between calls. Heap memory is
// bounded by the heap's limit, but a token spanning chunks is held outside it until it
// completes: check pending_bytes() against the limit after each feed.
class JsValueStreamDecoder {
public:
    JsValueStreamDecoder(GcHeap &heap, ValueHandle out) noexcept;
    ~JsValueStreamDecoder();

    JsValueStreamDecoder(const JsValueStreamDecoder &) = delete;
    JsValueStreamDecoder &operator=(const JsValueStreamDecoder &) = delete;
    JsValueStreamDecoder(JsValueStreamDecoder &&) = delete;
    JsValueStreamDecoder &operator=(JsValueStreamDecoder &&) = delete;

    // Call outside any NoGcScope: a chunk that may not fit first collects to make room.
    [[nodiscard]] DecodeStatus feed(const char *data, std::size_t len) noexcept;
    // Required even after feed reports Complete: what follows a complete value may be an
    // unfinished token that only the end of input can reject.
    [[nodiscard]] DecodeStatus finish() noexcept;

    [[nodiscard]] std::size_t pending_bytes() const noexcept;
    // Meaningful once feed or finish has failed; out is then undefined.
    [[nodiscard]] const ParseError &error() const noexcept { return error_; }

private:
    struct State;

    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] DecodeStatus settle(DecodeStatus status) noexcept;

    GcHeap &heap_;
    ValueHandle out_;
    std::unique_ptr<State> state_;
    ParseError error_{};
    DecodeStatus status_ = DecodeStatus::NeedMore;
};

} // namespace fiber::script::json

#endif // FIBER_JSVALUEDECODE_H
