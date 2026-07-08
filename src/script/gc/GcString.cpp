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

GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
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
    return str;
}

GcString *gc_new_string_bytes_uninit(GcHeap *heap, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
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
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
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
    return str;
}

GcString *gc_new_string_utf16_uninit(GcHeap *heap, std::size_t len) noexcept {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcHeapKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
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
