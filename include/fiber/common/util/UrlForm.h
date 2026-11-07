#ifndef FIBER_UTIL_URL_FORM_H
#define FIBER_UTIL_URL_FORM_H

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../IoError.h"
#include "../json/Utf.h"

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

// Decodes a single component into out (replaced). Returns false on a malformed percent
// escape, leaving out empty; true on success (out is valid UTF-8 after repair).
[[nodiscard]] bool form_decode_into(std::string_view in, std::string &out);

// First phase of a decode: percent-decodes in into dst, which must hold in.size() bytes
// (decoding never grows the input), and stores the decoded size in len. Returns false on a
// malformed percent escape. The result may still be malformed UTF-8; repairing it is the
// second phase (utf8_repaired_size, utf8_repair_into).
[[nodiscard]] bool form_percent_decode(std::string_view in, char *dst, std::size_t &len) noexcept;

// Decodes a query string into (key, value) pairs, invoking sink(key, value) per pair.
// sink returns true to continue, false to abort (yields IoErr::NoMem). A malformed
// percent escape yields IoErr::Invalid. Splitting mirrors Java UrlEncoded.decodeUtf8To:
// '&' separates pairs, the first '=' separates key/value, a segment without '=' becomes
// (segment, "") when non-empty and is skipped when empty, and '=' after the first is a
// literal part of the value. key/value are string_views into scratch buffers valid only
// for the duration of the sink call.
//
// alloc is called as alloc(size), with size > 0, and returns a writable buffer of that
// many bytes that stays valid until this call returns, or nullptr to refuse (yields
// IoErr::NoMem). One in.size() buffer serves every pair; a component that needs UTF-8
// repair takes one more, of its exact repaired size. Nothing else allocates, so noexcept
// holds exactly when calling alloc and sink does.
template<typename Alloc, typename Sink>
[[nodiscard]] fiber::common::IoResult<void> form_decode_query(std::string_view in, Alloc &&alloc, Sink &&sink) noexcept(
        noexcept(alloc(std::size_t{})) && noexcept(sink(std::string_view{}, std::string_view{})));

// Builds a query string by pulling pairs from source. source(key, value) fills the two
// strings and returns true when a pair was produced, false when exhausted. Pairs are
// form-encoded and joined with '&' (no trailing '&'). Appends to out. Never fails.
template<typename Source>
void form_build_query(std::string &out, Source &&source);

// ---- template implementations ----

namespace detail {

// Decodes one component into dst, which must hold in.size() bytes, moving it to an alloc
// buffer only when UTF-8 repair is needed.
template<typename Alloc>
[[nodiscard]] fiber::common::IoResult<std::string_view>
form_decode_component(std::string_view in, char *dst, Alloc &alloc) noexcept(noexcept(alloc(std::size_t{}))) {
    std::size_t len = 0;
    if (!form_percent_decode(in, dst, len)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    const std::string_view decoded(dst, len);
    if (fiber::json::utf8_validate(dst, len)) {
        return decoded;
    }
    const std::size_t repaired_size = fiber::json::utf8_repaired_size(decoded);
    char *repaired = alloc(repaired_size);
    if (repaired == nullptr) {
        return std::unexpected(fiber::common::IoErr::NoMem);
    }
    fiber::json::utf8_repair_into(decoded, repaired);
    return std::string_view(repaired, repaired_size);
}

} // namespace detail

template<typename Alloc, typename Sink>
fiber::common::IoResult<void> form_decode_query(std::string_view in, Alloc &&alloc, Sink &&sink) noexcept(
        noexcept(alloc(std::size_t{})) && noexcept(sink(std::string_view{}, std::string_view{}))) {
    static_assert(std::is_convertible_v<decltype(alloc(std::size_t{})), char *>,
                  "alloc must be callable as char *(std::size_t)");
    if (in.empty()) {
        return {};
    }
    // A pair's key and value are disjoint parts of in, so one buffer holds both.
    char *scratch = alloc(in.size());
    if (scratch == nullptr) {
        return std::unexpected(fiber::common::IoErr::NoMem);
    }
    const std::size_t len = in.size();
    std::size_t pos = 0;
    while (pos <= len) {
        std::size_t seg_end = pos;
        while (seg_end < len && in[seg_end] != '&') {
            ++seg_end;
        }
        const std::string_view seg(in.data() + pos, seg_end - pos);
        const std::size_t eq = seg.find('=');
        if (eq != std::string_view::npos) {
            std::string_view raw_key = seg;
            raw_key.remove_suffix(seg.size() - eq);
            std::string_view raw_value = seg;
            raw_value.remove_prefix(eq + 1);
            auto key = detail::form_decode_component(raw_key, scratch, alloc);
            if (!key) {
                return std::unexpected(key.error());
            }
            auto value = detail::form_decode_component(raw_value, scratch + raw_key.size(), alloc);
            if (!value) {
                return std::unexpected(value.error());
            }
            if (!sink(*key, *value)) {
                return std::unexpected(fiber::common::IoErr::NoMem);
            }
        } else {
            // No '=': the segment is a key with an empty value, but only when non-empty.
            auto key = detail::form_decode_component(seg, scratch, alloc);
            if (!key) {
                return std::unexpected(key.error());
            }
            if (!key->empty()) {
                if (!sink(*key, std::string_view{})) {
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
