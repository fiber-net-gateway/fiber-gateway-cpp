#ifndef FIBER_UTIL_URL_FORM_H
#define FIBER_UTIL_URL_FORM_H

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "../IoError.h"

namespace fiber::util {

// application/x-www-form-urlencoded codec (Jetty UrlEncoded semantics), operating
// entirely on UTF-8 byte strings. No JsValue/GcHeap dependency lives here; the
// script adapter (src/script/std/UrlFuncs) is responsible for JsValue <-> string.
//
// Encoding rules:
//   - unreserved [A-Za-z0-9 * - . _] pass through verbatim
//   - space (' ') -> '+'
//   - every other byte -> uppercase %XX (the input is treated as UTF-8, so a
//     multi-byte char is escaped byte-by-byte, matching Java's char->getBytes(UTF_8))
//
// Decoding rules:
//   - '+' -> space (0x20)
//   - %XX -> one byte; an incomplete or non-hex escape is an error (IoErr::Invalid)
//   - malformed UTF-8 in the decoded bytes is replaced with U+FFFD (mirrors Java's
//     REPLACE decoder), so the result is always valid UTF-8

// Encodes a single component, appending to out. Never fails.
void form_encode(std::string_view in, std::string &out);

// Decodes a single component. Returns IoErr::Invalid on a malformed percent escape.
[[nodiscard]] fiber::common::IoResult<std::string> form_decode(std::string_view in);

// Decodes a single component into out (cleared first). Returns false on a malformed
// percent escape; true on success (out is valid UTF-8 after repair). Shared core used
// by form_decode and form_decode_query.
[[nodiscard]] bool form_decode_into(std::string_view in, std::string &out);

// Decodes a query string into (key, value) pairs, invoking sink(key, value) per pair.
// sink returns true to continue, false to abort (yields IoErr::NoMem). A malformed
// percent escape yields IoErr::Invalid. Splitting mirrors Java UrlEncoded.decodeUtf8To:
// '&' separates pairs, the first '=' separates key/value, a segment without '=' becomes
// (segment, "") when non-empty and is skipped when empty, and '=' after the first is a
// literal part of the value. key/value are string_views into scratch buffers valid only
// for the duration of the sink call.
template<typename Sink>
[[nodiscard]] fiber::common::IoResult<void> form_decode_query(std::string_view in, Sink &&sink);

// Builds a query string by pulling pairs from source. source(key, value) fills the two
// strings and returns true when a pair was produced, false when exhausted. Pairs are
// form-encoded and joined with '&' (no trailing '&'). Appends to out. Never fails.
template<typename Source>
void form_build_query(std::string &out, Source &&source);

// ---- template implementations ----

template<typename Sink>
fiber::common::IoResult<void> form_decode_query(std::string_view in, Sink &&sink) {
    std::string key_buf;
    std::string val_buf;
    const std::size_t len = in.size();
    std::size_t pos = 0;
    while (pos <= len) {
        std::size_t seg_end = pos;
        while (seg_end < len && in[seg_end] != '&') {
            ++seg_end;
        }
        std::string_view seg = in.substr(pos, seg_end - pos);
        std::size_t eq = seg.find('=');
        if (eq != std::string_view::npos) {
            if (!form_decode_into(seg.substr(0, eq), key_buf)) {
                return std::unexpected(fiber::common::IoErr::Invalid);
            }
            if (!form_decode_into(seg.substr(eq + 1), val_buf)) {
                return std::unexpected(fiber::common::IoErr::Invalid);
            }
            if (!sink(std::string_view(key_buf), std::string_view(val_buf))) {
                return std::unexpected(fiber::common::IoErr::NoMem);
            }
        } else {
            // No '=': the segment is a key with an empty value, but only when non-empty.
            if (!form_decode_into(seg, key_buf)) {
                return std::unexpected(fiber::common::IoErr::Invalid);
            }
            if (!key_buf.empty()) {
                if (!sink(std::string_view(key_buf), std::string_view{})) {
                    return std::unexpected(fiber::common::IoErr::NoMem);
                }
            }
        }
        if (seg_end == len) {
            break;
        }
        pos = seg_end + 1; // skip the '&'
    }
    return {};
}

template<typename Source>
void form_build_query(std::string &out, Source &&source) {
    std::string key;
    std::string value;
    bool first = true;
    while (source(key, value)) {
        if (!first) {
            out.push_back('&');
        }
        first = false;
        form_encode(key, out);
        out.push_back('=');
        form_encode(value, out);
    }
}

} // namespace fiber::util

#endif // FIBER_UTIL_URL_FORM_H
