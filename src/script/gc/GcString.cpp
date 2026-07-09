//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include "../../common/Assert.h"
#include "../../common/json/Utf.h"

#include <cstring>
#include <string>

namespace fiber::script {

using namespace gc_detail;

namespace {

bool is_high_surrogate(char16_t unit) noexcept { return unit >= 0xD800 && unit <= 0xDBFF; }

bool is_low_surrogate(char16_t unit) noexcept { return unit >= 0xDC00 && unit <= 0xDFFF; }

std::uint8_t write_utf8_codepoint(std::uint32_t codepoint, char *out) noexcept {
    if (codepoint < 0x80) {
        out[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 4;
}

GcStringUtf8Result utf8_result(GcStringUtf8Status status, std::size_t written, std::size_t needed = 0) noexcept {
    return {.status = status, .written = written, .needed = needed};
}

} // namespace

GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    std::uint64_t hash = 0;
    const bool intern = len <= kMaxInternStringLen;
    if (intern) {
        hash = string_hash_bytes(data, len);
        if (GcString *existing = gc_string_intern_lookup_bytes(heap, data, len, hash)) {
            return existing;
        }
    }
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->intern_next = nullptr;
    str->len = len;
    str->encoding = GcStringEncoding::Byte;
    str->hash = 0;
    str->hash_valid = false;
    str->data8 = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(str);
        return nullptr;
    }
    if (len > 0) {
        str->data8 =
                static_cast<std::uint8_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Byte)));
        if (!str->data8) {
            heap->alloc.free(str);
            return nullptr;
        }
        std::memcpy(str->data8, data, len);
        str->data8[len] = 0;
    }
    gc_link(heap, hdr);
    if (intern) {
        gc_string_intern_insert(heap, str, hash);
    }
    return str;
}

GcString *gc_new_string_bytes_uninit(GcHeap *heap, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->intern_next = nullptr;
    str->len = len;
    str->encoding = GcStringEncoding::Byte;
    str->hash = 0;
    str->hash_valid = false;
    str->data8 = nullptr;
    if (len > 0) {
        str->data8 =
                static_cast<std::uint8_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Byte)));
        if (!str->data8) {
            heap->alloc.free(str);
            return nullptr;
        }
        str->data8[len] = 0;
    }
    gc_link(heap, hdr);
    return str;
}

GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    std::uint64_t hash = 0;
    const bool intern = len <= kMaxInternStringLen;
    if (intern) {
        hash = string_hash_utf16(data, len);
        if (GcString *existing = gc_string_intern_lookup_utf16(heap, data, len, hash)) {
            return existing;
        }
    }
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->intern_next = nullptr;
    str->len = len;
    str->encoding = GcStringEncoding::Utf16;
    str->hash = 0;
    str->hash_valid = false;
    str->data16 = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(str);
        return nullptr;
    }
    if (len > 0) {
        str->data16 = static_cast<char16_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Utf16)));
        if (!str->data16) {
            heap->alloc.free(str);
            return nullptr;
        }
        std::memcpy(str->data16, data, sizeof(char16_t) * len);
        str->data16[len] = 0;
    }
    gc_link(heap, hdr);
    if (intern) {
        gc_string_intern_insert(heap, str, hash);
    }
    return str;
}

GcString *gc_new_string_utf16_uninit(GcHeap *heap, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->intern_next = nullptr;
    str->len = len;
    str->encoding = GcStringEncoding::Utf16;
    str->hash = 0;
    str->hash_valid = false;
    str->data16 = nullptr;
    if (len > 0) {
        str->data16 = static_cast<char16_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Utf16)));
        if (!str->data16) {
            heap->alloc.free(str);
            return nullptr;
        }
        str->data16[len] = 0;
    }
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
    if (scan.utf16_len <= kMaxInternStringLen) {
        if (scan.all_byte) {
            std::uint8_t decoded[kMaxInternStringLen];
            if (!fiber::json::utf8_write_bytes(data, len, decoded, scan.utf16_len)) {
                return nullptr;
            }
            std::uint64_t hash = string_hash_bytes(decoded, scan.utf16_len);
            if (GcString *existing = gc_string_intern_lookup_bytes(heap, decoded, scan.utf16_len, hash)) {
                return existing;
            }
            GcString *str = gc_new_string_bytes_uninit(heap, scan.utf16_len);
            if (!str) {
                return nullptr;
            }
            if (scan.utf16_len > 0) {
                std::memcpy(str->data8, decoded, scan.utf16_len);
            }
            gc_string_intern_insert(heap, str, hash);
            return str;
        }

        char16_t decoded[kMaxInternStringLen];
        if (!fiber::json::utf8_write_utf16(data, len, decoded, scan.utf16_len)) {
            return nullptr;
        }
        std::uint64_t hash = string_hash_utf16(decoded, scan.utf16_len);
        if (GcString *existing = gc_string_intern_lookup_utf16(heap, decoded, scan.utf16_len, hash)) {
            return existing;
        }
        GcString *str = gc_new_string_utf16_uninit(heap, scan.utf16_len);
        if (!str) {
            return nullptr;
        }
        if (scan.utf16_len > 0) {
            std::memcpy(str->data16, decoded, sizeof(char16_t) * scan.utf16_len);
        }
        gc_string_intern_insert(heap, str, hash);
        return str;
    }
    if (scan.all_byte) {
        GcString *str = gc_new_string_bytes_uninit(heap, scan.utf16_len);
        if (!str) {
            return nullptr;
        }
        bool written = fiber::json::utf8_write_bytes(data, len, str->data8, str->len);
        FIBER_ASSERT(written);
        (void) written;
        return str;
    }
    GcString *str = gc_new_string_utf16_uninit(heap, scan.utf16_len);
    if (!str) {
        return nullptr;
    }
    bool written = fiber::json::utf8_write_utf16(data, len, str->data16, str->len);
    FIBER_ASSERT(written);
    (void) written;
    return str;
}

bool gc_string_can_encode_utf8(const GcString *str) noexcept {
    if (!str) {
        return false;
    }
    if (str->len == 0) {
        return true;
    }
    if (str->encoding == GcStringEncoding::Byte) {
        return str->data8 != nullptr;
    }
    if (!str->data16) {
        return false;
    }
    for (std::size_t i = 0; i < str->len; ++i) {
        char16_t unit = str->data16[i];
        if (is_high_surrogate(unit)) {
            if (i + 1 >= str->len || !is_low_surrogate(str->data16[i + 1])) {
                return false;
            }
            i += 1;
            continue;
        }
        if (is_low_surrogate(unit)) {
            return false;
        }
    }
    return true;
}

GcStringUtf8Result gc_string_to_utf8(const GcString *str, GcStringUtf8Cursor &cursor, GcStringUtf8Buffer out,
                                     GcStringUtf8Boundary boundary) noexcept {
    if (!str || (out.capacity > 0 && !out.ptr) || cursor.index > str->len ||
        cursor.pending_offset > cursor.pending_len || cursor.pending_len > sizeof(cursor.pending)) {
        return utf8_result(GcStringUtf8Status::Invalid, 0);
    }
    if (str->len > 0) {
        if (str->encoding == GcStringEncoding::Byte) {
            if (!str->data8) {
                return utf8_result(GcStringUtf8Status::Invalid, 0);
            }
        } else if (!str->data16) {
            return utf8_result(GcStringUtf8Status::Invalid, 0);
        }
    }

    std::size_t written = 0;
    if (cursor.pending_offset < cursor.pending_len) {
        const std::size_t remaining = cursor.pending_len - cursor.pending_offset;
        const std::size_t space = out.capacity;
        if (remaining > space) {
            if (boundary == GcStringUtf8Boundary::PreserveCodePoint) {
                return utf8_result(GcStringUtf8Status::NeedMore, 0, remaining);
            }
            if (space == 0) {
                return utf8_result(GcStringUtf8Status::NeedMore, 0, remaining);
            }
            std::memcpy(out.ptr, cursor.pending + cursor.pending_offset, space);
            cursor.pending_offset = static_cast<std::uint8_t>(cursor.pending_offset + space);
            return utf8_result(GcStringUtf8Status::NeedMore, space, remaining - space);
        }
        if (remaining > 0) {
            std::memcpy(out.ptr, cursor.pending + cursor.pending_offset, remaining);
        }
        written = remaining;
        cursor.pending_offset = 0;
        cursor.pending_len = 0;
    }

    while (cursor.index < str->len) {
        char encoded[4];
        std::uint8_t encoded_len = 0;
        std::size_t consumed = 1;
        if (str->encoding == GcStringEncoding::Byte) {
            encoded_len = write_utf8_codepoint(str->data8[cursor.index], encoded);
        } else {
            char16_t unit = str->data16[cursor.index];
            if (is_high_surrogate(unit)) {
                if (cursor.index + 1 >= str->len) {
                    return utf8_result(GcStringUtf8Status::Invalid, written);
                }
                char16_t low = str->data16[cursor.index + 1];
                if (!is_low_surrogate(low)) {
                    return utf8_result(GcStringUtf8Status::Invalid, written);
                }
                std::uint32_t codepoint = 0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
                                          (static_cast<std::uint32_t>(low) - 0xDC00);
                encoded_len = write_utf8_codepoint(codepoint, encoded);
                consumed = 2;
            } else {
                if (is_low_surrogate(unit)) {
                    return utf8_result(GcStringUtf8Status::Invalid, written);
                }
                encoded_len = write_utf8_codepoint(unit, encoded);
            }
        }

        const std::size_t space = out.capacity - written;
        if (space < encoded_len) {
            if (boundary == GcStringUtf8Boundary::PreserveCodePoint) {
                return utf8_result(GcStringUtf8Status::NeedMore, written, encoded_len);
            }
            if (space == 0) {
                return utf8_result(GcStringUtf8Status::NeedMore, written, encoded_len);
            }
            std::memcpy(out.ptr + written, encoded, space);
            std::memcpy(cursor.pending, encoded, encoded_len);
            cursor.pending_offset = static_cast<std::uint8_t>(space);
            cursor.pending_len = encoded_len;
            cursor.index += consumed;
            written += space;
            return utf8_result(GcStringUtf8Status::NeedMore, written, encoded_len - space);
        }

        if (encoded_len > 0) {
            std::memcpy(out.ptr + written, encoded, encoded_len);
        }
        written += encoded_len;
        cursor.index += consumed;
    }

    return utf8_result(GcStringUtf8Status::Done, written);
}

bool gc_string_to_utf8(const GcString *str, std::string &out) {
    out.clear();
    if (!str) {
        return false;
    }
    if (str->len == 0) {
        return true;
    }
    if (str->encoding == GcStringEncoding::Byte) {
        std::size_t extra = 0;
        for (std::size_t i = 0; i < str->len; ++i) {
            if (str->data8[i] >= 0x80) {
                extra += 1;
            }
        }
        if (extra == 0) {
            out.assign(reinterpret_cast<const char *>(str->data8), str->len);
            return true;
        }
        out.resize(str->len + extra);
        char *dst = out.data();
        for (std::size_t i = 0; i < str->len; ++i) {
            std::uint8_t byte = str->data8[i];
            if (byte < 0x80) {
                *dst++ = static_cast<char>(byte);
            } else {
                *dst++ = static_cast<char>(0xC0 | (byte >> 6));
                *dst++ = static_cast<char>(0x80 | (byte & 0x3F));
            }
        }
        return true;
    }
    std::size_t out_len = 0;
    for (std::size_t i = 0; i < str->len; ++i) {
        char16_t unit = str->data16[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 >= str->len) {
                return false;
            }
            char16_t low = str->data16[i + 1];
            if (low < 0xDC00 || low > 0xDFFF) {
                return false;
            }
            out_len += 4;
            i += 1;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
        if (unit < 0x80) {
            out_len += 1;
        } else if (unit < 0x800) {
            out_len += 2;
        } else {
            out_len += 3;
        }
    }
    out.resize(out_len);
    char *dst = out.data();
    for (std::size_t i = 0; i < str->len; ++i) {
        char16_t unit = str->data16[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 >= str->len) {
                return false;
            }
            char16_t low = str->data16[i + 1];
            if (low < 0xDC00 || low > 0xDFFF) {
                return false;
            }
            std::uint32_t codepoint = 0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
                                      (static_cast<std::uint32_t>(low) - 0xDC00);
            *dst++ = static_cast<char>(0xF0 | (codepoint >> 18));
            *dst++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            *dst++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            *dst++ = static_cast<char>(0x80 | (codepoint & 0x3F));
            i += 1;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
        std::uint32_t codepoint = unit;
        if (codepoint < 0x80) {
            *dst++ = static_cast<char>(codepoint);
        } else if (codepoint < 0x800) {
            *dst++ = static_cast<char>(0xC0 | (codepoint >> 6));
            *dst++ = static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            *dst++ = static_cast<char>(0xE0 | (codepoint >> 12));
            *dst++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            *dst++ = static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    }
    return true;
}

} // namespace fiber::script
