//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"
#include "Wtf8.h"

#include "../../common/Assert.h"
#include "../../common/json/Utf.h"

#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace fiber::script {

using namespace gc_detail;

namespace {

GcStringUtf8Result utf8_result(GcStringUtf8Status status, std::size_t written, std::size_t needed = 0) noexcept {
    return {.status = status, .written = written, .needed = needed};
}

GcString *new_wtf8_copy(GcHeap *heap, const char *data, std::size_t byte_len, std::size_t utf16_len,
                        bool well_formed) noexcept {
    if ((byte_len > 0 && !data) || utf16_len > std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    const bool intern = utf16_len <= kMaxInternStringLen;
    std::uint64_t hash = 0;
    if (intern) {
        hash = string_hash_wtf8(data, byte_len);
        if (GcString *existing = gc_string_intern_lookup_wtf8(heap, data, byte_len, utf16_len, hash)) {
            return existing;
        }
    }

    GcString *str = gc_new_string_wtf8_uninit(heap, byte_len, utf16_len, well_formed);
    if (!str) {
        return nullptr;
    }
    if (byte_len > 0) {
        std::memcpy(gc_string_wtf8_data(str), data, byte_len);
    }
    if (intern) {
        gc_string_intern_insert(heap, str, hash);
    }
    return str;
}

} // namespace

GcString *gc_new_string_wtf8_uninit(GcHeap *heap, std::size_t byte_len, std::size_t utf16_len,
                                    bool well_formed) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    constexpr std::size_t overhead = sizeof(GcString) + 1;
    if (utf16_len > std::numeric_limits<std::uint32_t>::max() || byte_len > std::numeric_limits<std::uint32_t>::max() ||
        byte_len > std::numeric_limits<std::uint32_t>::max() - overhead) {
        return nullptr;
    }
    const std::size_t allocation_size = overhead + byte_len;
    auto *hdr = gc_alloc_raw(heap, allocation_size, GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->intern_next = nullptr;
    str->hash = 0;
    str->utf16_len = static_cast<std::uint32_t>(utf16_len);
    str->flags = well_formed ? kGcStringWellFormed : 0;
    gc_string_wtf8_data(str)[byte_len] = 0;
    gc_link(heap, hdr);
    return str;
}

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len) noexcept {
    if (len > 0 && !data) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    fiber::json::Utf8ScanResult scan;
    if (!fiber::json::utf8_scan(data, len, scan)) {
        return nullptr;
    }
    return new_wtf8_copy(heap, data, len, scan.utf16_len, true);
}

GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    std::size_t byte_len = 0;
    if (!wtf8_measure_latin1(data, len, byte_len)) {
        return nullptr;
    }

    if (len <= kMaxInternStringLen) {
        char encoded[kMaxInternStringLen * 2];
        if (!wtf8_write_latin1(data, len, encoded, byte_len)) {
            return nullptr;
        }
        return new_wtf8_copy(heap, encoded, byte_len, len, true);
    }

    GcString *str = gc_new_string_wtf8_uninit(heap, byte_len, len, true);
    if (!str || !wtf8_write_latin1(data, len, gc_string_wtf8_data(str), byte_len)) {
        return nullptr;
    }
    return str;
}

GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    Wtf8MeasureResult measure;
    if (!wtf8_measure_utf16(data, len, measure)) {
        return nullptr;
    }

    if (len <= kMaxInternStringLen) {
        char encoded[kMaxInternStringLen * 3];
        if (!wtf8_write_utf16(data, len, encoded, measure.byte_len)) {
            return nullptr;
        }
        return new_wtf8_copy(heap, encoded, measure.byte_len, len, measure.well_formed);
    }

    GcString *str = gc_new_string_wtf8_uninit(heap, measure.byte_len, len, measure.well_formed);
    if (!str || !wtf8_write_utf16(data, len, gc_string_wtf8_data(str), measure.byte_len)) {
        return nullptr;
    }
    return str;
}

GcString *gc_new_string_substring_utf16(GcHeap *heap, const GcString *source, std::size_t begin,
                                        std::size_t end) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    if (!source || begin > end || end > source->utf16_len) {
        return nullptr;
    }
    if (begin == end) {
        return gc_new_string(heap, "", 0);
    }

    const char *source_data = gc_string_wtf8_data(source);
    if (gc_string_is_ascii(source)) {
        return new_wtf8_copy(heap, source_data + begin, end - begin, end - begin, true);
    }
    const std::size_t source_len = gc_string_byte_len(source);
    Wtf8SlicePlan plan;
    if (!wtf8_plan_utf16_slice(source_data, source_len, source->utf16_len, begin, end, plan) ||
        plan.copy_begin > plan.copy_end || plan.copy_end > source_len) {
        return nullptr;
    }

    std::size_t byte_len = plan.copy_end - plan.copy_begin;
    const std::size_t boundary_bytes =
            (plan.leading_low_surrogate ? 3u : 0u) + (plan.trailing_high_surrogate ? 3u : 0u);
    if (byte_len > std::numeric_limits<std::size_t>::max() - boundary_bytes) {
        return nullptr;
    }
    byte_len += boundary_bytes;

    GcString *str = gc_new_string_wtf8_uninit(heap, byte_len, end - begin, false);
    if (!str) {
        return nullptr;
    }
    char *dst = gc_string_wtf8_data(str);
    std::size_t offset = 0;
    if (plan.leading_low_surrogate) {
        offset += wtf8_write_codepoint(plan.leading_unit, dst + offset);
    }
    const std::size_t copy_len = plan.copy_end - plan.copy_begin;
    if (copy_len > 0) {
        std::memcpy(dst + offset, source_data + plan.copy_begin, copy_len);
        offset += copy_len;
    }
    if (plan.trailing_high_surrogate) {
        offset += wtf8_write_codepoint(plan.trailing_unit, dst + offset);
    }
    FIBER_ASSERT(offset == byte_len);
    if (wtf8_is_well_formed(dst, byte_len)) {
        str->flags |= kGcStringWellFormed;
    }
    return gc_string_intern_final(heap, str);
}

bool gc_string_can_encode_utf8(const GcString *str) noexcept { return gc_string_is_well_formed(str); }

bool gc_string_utf8_view(const GcString *str, std::string_view &out) noexcept {
    out = {};
    if (!gc_string_is_well_formed(str)) {
        return false;
    }
    out = std::string_view(gc_string_wtf8_data(str), gc_string_byte_len(str));
    return true;
}

GcStringUtf8Result gc_string_to_utf8(const GcString *str, GcStringUtf8Cursor &cursor, GcStringUtf8Buffer out,
                                     GcStringUtf8Boundary boundary) noexcept {
    if (!str || !gc_string_is_well_formed(str) || (out.capacity > 0 && !out.ptr) ||
        cursor.index > gc_string_byte_len(str)) {
        return utf8_result(GcStringUtf8Status::Invalid, 0);
    }

    const char *data = gc_string_wtf8_data(str);
    const std::size_t len = gc_string_byte_len(str);
    if (boundary == GcStringUtf8Boundary::AllowSplitCodePoint) {
        const std::size_t remaining = len - cursor.index;
        const std::size_t written = remaining < out.capacity ? remaining : out.capacity;
        if (written > 0) {
            std::memcpy(out.ptr, data + cursor.index, written);
            cursor.index += written;
        }
        return cursor.index == len ? utf8_result(GcStringUtf8Status::Done, written)
                                   : utf8_result(GcStringUtf8Status::NeedMore, written, len - cursor.index);
    }

    std::size_t written = 0;
    while (cursor.index < len) {
        std::size_t next = cursor.index;
        std::uint32_t codepoint = 0;
        if (!fiber::json::utf8_next_codepoint(data, len, next, codepoint)) {
            return utf8_result(GcStringUtf8Status::Invalid, written);
        }
        const std::size_t encoded_len = next - cursor.index;
        if (encoded_len > out.capacity - written) {
            return utf8_result(GcStringUtf8Status::NeedMore, written, encoded_len);
        }
        std::memcpy(out.ptr + written, data + cursor.index, encoded_len);
        written += encoded_len;
        cursor.index = next;
    }
    return utf8_result(GcStringUtf8Status::Done, written);
}

bool gc_string_to_utf8(const GcString *str, std::string &out) {
    out.clear();
    std::string_view view;
    if (!gc_string_utf8_view(str, view)) {
        return false;
    }
    out.assign(view.data(), view.size());
    return true;
}

} // namespace fiber::script
