//
// Created by dear on 2025/12/30.
//

#include "JsGc.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fiber::json {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kMinBucketCount = 8;
constexpr std::size_t kMaxLoadNumerator = 3;
constexpr std::size_t kMaxLoadDenominator = 4;

GcMark flip_mark(GcMark mark) {
    return (mark == GcMark::GcMark_0) ? GcMark::GcMark_1 : GcMark::GcMark_0;
}

std::uint64_t hash_code_units(const GcString *str) {
    std::uint64_t hash = kFnvOffsetBasis;
    if (!str || str->len == 0) {
        return hash;
    }
    if (str->encoding == GcStringEncoding::Byte) {
        for (std::size_t i = 0; i < str->len; ++i) {
            hash ^= static_cast<std::uint16_t>(str->data8[i]);
            hash *= kFnvPrime;
        }
        return hash;
    }
    for (std::size_t i = 0; i < str->len; ++i) {
        hash ^= static_cast<std::uint16_t>(str->data16[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t string_hash(const GcString *str) {
    if (!str) {
        return 0;
    }
    if (str->hash_valid) {
        return str->hash;
    }
    auto *mutable_str = const_cast<GcString *>(str);
    mutable_str->hash = hash_code_units(str);
    mutable_str->hash_valid = true;
    return mutable_str->hash;
}

bool string_equals(const GcString *lhs, const GcString *rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (!lhs || !rhs) {
        return false;
    }
    if (lhs->len != rhs->len) {
        return false;
    }
    if (lhs->len == 0) {
        return true;
    }
    if (lhs->encoding == rhs->encoding) {
        if (lhs->encoding == GcStringEncoding::Byte) {
            return std::memcmp(lhs->data8, rhs->data8, lhs->len) == 0;
        }
        return std::memcmp(lhs->data16, rhs->data16, lhs->len * sizeof(char16_t)) == 0;
    }
    if (lhs->encoding == GcStringEncoding::Byte) {
        for (std::size_t i = 0; i < lhs->len; ++i) {
            if (rhs->data16[i] != static_cast<char16_t>(lhs->data8[i])) {
                return false;
            }
        }
        return true;
    }
    for (std::size_t i = 0; i < lhs->len; ++i) {
        if (lhs->data16[i] != static_cast<char16_t>(rhs->data8[i])) {
            return false;
        }
    }
    return true;
}

std::size_t next_pow2(std::size_t value) {
    if (value <= 1) {
        return 1;
    }
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

std::size_t bucket_count_for_entries(std::size_t entry_capacity) {
    if (entry_capacity == 0) {
        return 0;
    }
    std::size_t needed =
        (entry_capacity * kMaxLoadDenominator + kMaxLoadNumerator - 1) / kMaxLoadNumerator;
    if (needed < kMinBucketCount) {
        needed = kMinBucketCount;
    }
    return next_pow2(needed);
}

struct DecodedString {
    bool is_byte = true;
    std::vector<std::uint8_t> bytes;
    std::vector<char16_t> u16;
};

void append_code_unit(DecodedString &out, char16_t unit) {
    if (out.is_byte && unit <= 0xFF) {
        out.bytes.push_back(static_cast<std::uint8_t>(unit));
        return;
    }
    if (out.is_byte) {
        out.is_byte = false;
        out.u16.reserve(out.bytes.size() + 1);
        for (std::uint8_t byte : out.bytes) {
            out.u16.push_back(static_cast<char16_t>(byte));
        }
        out.bytes.clear();
    }
    out.u16.push_back(unit);
}

void append_codepoint(DecodedString &out, std::uint32_t codepoint) {
    if (codepoint <= 0xFFFF) {
        append_code_unit(out, static_cast<char16_t>(codepoint));
        return;
    }
    std::uint32_t value = codepoint - 0x10000;
    char16_t high = static_cast<char16_t>(0xD800 + (value >> 10));
    char16_t low = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
    append_code_unit(out, high);
    append_code_unit(out, low);
}

bool decode_utf8(const char *data, std::size_t len, DecodedString &out) {
    std::size_t pos = 0;
    while (pos < len) {
        unsigned char ch = static_cast<unsigned char>(data[pos]);
        if (ch < 0x80) {
            append_code_unit(out, static_cast<char16_t>(ch));
            pos += 1;
            continue;
        }
        int needed = 0;
        std::uint32_t code = 0;
        std::uint32_t min_value = 0;
        if ((ch & 0xE0) == 0xC0) {
            needed = 1;
            code = ch & 0x1F;
            min_value = 0x80;
        } else if ((ch & 0xF0) == 0xE0) {
            needed = 2;
            code = ch & 0x0F;
            min_value = 0x800;
        } else if ((ch & 0xF8) == 0xF0) {
            needed = 3;
            code = ch & 0x07;
            min_value = 0x10000;
        } else {
            return false;
        }
        if (pos + static_cast<std::size_t>(needed) >= len) {
            return false;
        }
        for (int idx = 1; idx <= needed; ++idx) {
            unsigned char next = static_cast<unsigned char>(data[pos + idx]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < min_value || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        append_codepoint(out, code);
        pos += static_cast<std::size_t>(needed) + 1;
    }
    return true;
}

void append_utf8(std::string &out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

int32_t find_entry_index(const GcObject *obj, const GcString *key, std::uint64_t hash) {
    if (!obj || obj->bucket_count == 0 || !obj->buckets) {
        return -1;
    }
    std::size_t bucket = static_cast<std::size_t>(hash) & obj->bucket_mask;
    int32_t idx = obj->buckets[bucket];
    while (idx != -1) {
        const GcObjectEntry &entry = obj->entries[idx];
        if (entry.occupied && entry.hash == hash && string_equals(entry.key, key)) {
            return idx;
        }
        idx = entry.next_bucket;
    }
    return -1;
}

bool rehash_buckets(GcHeap *heap, GcObject *obj, std::size_t new_bucket_count) {
    if (!heap || !obj) {
        return false;
    }
    if (new_bucket_count == 0) {
        return false;
    }
    auto *new_buckets =
        static_cast<std::int32_t *>(heap->alloc.alloc(sizeof(std::int32_t) * new_bucket_count));
    if (!new_buckets) {
        return false;
    }
    for (std::size_t i = 0; i < new_bucket_count; ++i) {
        new_buckets[i] = -1;
    }
    for (std::size_t i = 0; i < obj->entry_count; ++i) {
        GcObjectEntry &entry = obj->entries[i];
        if (!entry.occupied) {
            entry.next_bucket = -1;
            continue;
        }
        std::size_t bucket = static_cast<std::size_t>(entry.hash) & (new_bucket_count - 1);
        entry.next_bucket = new_buckets[bucket];
        new_buckets[bucket] = static_cast<std::int32_t>(i);
    }
    if (obj->buckets) {
        heap->alloc.free(obj->buckets);
    }
    obj->buckets = new_buckets;
    obj->bucket_count = new_bucket_count;
    obj->bucket_mask = new_bucket_count - 1;
    return true;
}

bool grow_entries(GcHeap *heap, GcObject *obj, std::size_t new_capacity) {
    if (!heap || !obj || new_capacity == 0) {
        return false;
    }
    auto *new_entries =
        static_cast<GcObjectEntry *>(heap->alloc.alloc(sizeof(GcObjectEntry) * new_capacity));
    if (!new_entries) {
        return false;
    }
    for (std::size_t i = 0; i < new_capacity; ++i) {
        new_entries[i].key = nullptr;
        new_entries[i].hash = 0;
        new_entries[i].next_bucket = -1;
        new_entries[i].prev_order = -1;
        new_entries[i].next_order = -1;
        new_entries[i].next_free = -1;
        new_entries[i].occupied = false;
        std::construct_at(&new_entries[i].value);
    }
    for (std::size_t i = 0; i < obj->entry_count; ++i) {
        new_entries[i].key = obj->entries[i].key;
        new_entries[i].hash = obj->entries[i].hash;
        new_entries[i].next_bucket = obj->entries[i].next_bucket;
        new_entries[i].prev_order = obj->entries[i].prev_order;
        new_entries[i].next_order = obj->entries[i].next_order;
        new_entries[i].next_free = obj->entries[i].next_free;
        new_entries[i].occupied = obj->entries[i].occupied;
        new_entries[i].value = std::move(obj->entries[i].value);
    }
    if (obj->entries) {
        for (std::size_t i = 0; i < obj->entry_capacity; ++i) {
            std::destroy_at(&obj->entries[i].value);
        }
        heap->alloc.free(obj->entries);
    }
    obj->entries = new_entries;
    obj->entry_capacity = new_capacity;
    return true;
}

int32_t allocate_entry(GcObject *obj) {
    if (!obj) {
        return -1;
    }
    if (obj->free_head != -1) {
        int32_t idx = obj->free_head;
        obj->free_head = obj->entries[idx].next_free;
        obj->entries[idx].next_free = -1;
        return idx;
    }
    if (obj->entry_count >= obj->entry_capacity) {
        return -1;
    }
    int32_t idx = static_cast<int32_t>(obj->entry_count);
    obj->entry_count += 1;
    return idx;
}

GcHeader *gc_alloc_raw(GcHeap *heap, std::size_t size, GcKind kind) {
    void *mem = heap->alloc.alloc(size);
    if (!mem) {
        return nullptr;
    }
    auto *hdr = static_cast<GcHeader *>(mem);
    hdr->next = nullptr;
    hdr->mark_ = flip_mark(heap->live_mark);
    hdr->kind = kind;
    hdr->size_ = static_cast<std::uint32_t>(size);
    return hdr;
}

void gc_link(GcHeap *heap, GcHeader *hdr) {
    hdr->next = heap->head;
    heap->head = hdr;
    heap->bytes += hdr->size_;
}

void gc_mark_obj(GcHeap *heap, GcHeader *obj);

void gc_mark_value(GcHeap *heap, const JsValue &value) {
    switch (value.type_) {
        case JsNodeType::HeapString:
        case JsNodeType::HeapBinary:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Exception:
        case JsNodeType::Interator:
            if (value.gc) {
                gc_mark_obj(heap, value.gc);
            }
            break;
        default:
            break;
    }
}

void gc_mark_obj(GcHeap *heap, GcHeader *obj) {
    if (!obj || obj->mark_ == heap->live_mark) {
        return;
    }
    obj->mark_ = heap->live_mark;
    switch (obj->kind) {
        case GcKind::String:
            break;
        case GcKind::Binary:
            break;
        case GcKind::Array: {
            auto *arr = reinterpret_cast<GcArray *>(obj);
            for (std::size_t i = 0; i < arr->size; ++i) {
                gc_mark_value(heap, arr->elems[i]);
            }
            break;
        }
        case GcKind::Object: {
            auto *objv = reinterpret_cast<GcObject *>(obj);
            int32_t cursor = objv->head;
            while (cursor != -1) {
                const GcObjectEntry &entry = objv->entries[cursor];
                if (entry.key) {
                    gc_mark_obj(heap, &entry.key->hdr);
                }
                gc_mark_value(heap, entry.value);
                cursor = entry.next_order;
            }
            break;
        }
        case GcKind::Exception:
        case GcKind::Iterator:
            break;
    }
}

void gc_free_obj(GcHeap *heap, GcHeader *obj) {
    switch (obj->kind) {
        case GcKind::String: {
            auto *str = reinterpret_cast<GcString *>(obj);
            if (str->encoding == GcStringEncoding::Utf16) {
                if (str->data16) {
                    heap->alloc.free(str->data16);
                }
            } else if (str->data8) {
                heap->alloc.free(str->data8);
            }
            break;
        }
        case GcKind::Binary: {
            auto *bin = reinterpret_cast<GcBinary *>(obj);
            if (bin->data) {
                heap->alloc.free(bin->data);
            }
            break;
        }
        case GcKind::Array: {
            auto *arr = reinterpret_cast<GcArray *>(obj);
            if (arr->elems) {
                for (std::size_t i = 0; i < arr->capacity; ++i) {
                    std::destroy_at(&arr->elems[i]);
                }
                heap->alloc.free(arr->elems);
            }
            break;
        }
        case GcKind::Object: {
            auto *objv = reinterpret_cast<GcObject *>(obj);
            if (objv->entries) {
                for (std::size_t i = 0; i < objv->entry_capacity; ++i) {
                    std::destroy_at(&objv->entries[i].value);
                }
                heap->alloc.free(objv->entries);
            }
            if (objv->buckets) {
                heap->alloc.free(objv->buckets);
            }
            break;
        }
        case GcKind::Exception:
        case GcKind::Iterator:
            break;
    }
    heap->bytes -= obj->size_;
    heap->alloc.free(obj);
}

} // namespace

GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
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
        str->data8 = static_cast<std::uint8_t *>(heap->alloc.alloc(len + 1));
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

GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
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
        str->data16 = static_cast<char16_t *>(heap->alloc.alloc(sizeof(char16_t) * (len + 1)));
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

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len) {
    if (len > 0 && !data) {
        return nullptr;
    }
    DecodedString decoded;
    if (len > 0 && !decode_utf8(data, len, decoded)) {
        return nullptr;
    }
    if (decoded.is_byte) {
        return gc_new_string_bytes(heap, decoded.bytes.data(), decoded.bytes.size());
    }
    return gc_new_string_utf16(heap, decoded.u16.data(), decoded.u16.size());
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
            std::uint32_t codepoint = 0x10000 +
                                      ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
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

GcBinary *gc_new_binary(GcHeap *heap, const std::uint8_t *data, std::size_t len) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcBinary), GcKind::Binary);
    if (!hdr) {
        return nullptr;
    }
    auto *bin = reinterpret_cast<GcBinary *>(hdr);
    bin->len = len;
    bin->data = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(bin);
        return nullptr;
    }
    if (len > 0) {
        bin->data = static_cast<std::uint8_t *>(heap->alloc.alloc(len));
        if (!bin->data) {
            heap->alloc.free(bin);
            return nullptr;
        }
        std::memcpy(bin->data, data, len);
    }
    gc_link(heap, hdr);
    return bin;
}

GcArray *gc_new_array(GcHeap *heap, std::size_t capacity) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcArray), GcKind::Array);
    if (!hdr) {
        return nullptr;
    }
    auto *arr = reinterpret_cast<GcArray *>(hdr);
    arr->size = 0;
    arr->capacity = capacity;
    arr->elems = nullptr;
    if (capacity > 0) {
        arr->elems = static_cast<JsValue *>(heap->alloc.alloc(sizeof(JsValue) * capacity));
        if (!arr->elems) {
            heap->alloc.free(arr);
            return nullptr;
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            std::construct_at(&arr->elems[i]);
        }
    }
    gc_link(heap, hdr);
    return arr;
}

GcObject *gc_new_object(GcHeap *heap, std::size_t capacity) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcObject), GcKind::Object);
    if (!hdr) {
        return nullptr;
    }
    auto *obj = reinterpret_cast<GcObject *>(hdr);
    obj->size = 0;
    obj->entry_count = 0;
    obj->entry_capacity = capacity;
    obj->bucket_count = 0;
    obj->bucket_mask = 0;
    obj->head = -1;
    obj->tail = -1;
    obj->free_head = -1;
    obj->buckets = nullptr;
    obj->entries = nullptr;
    if (capacity > 0) {
        obj->entries = static_cast<GcObjectEntry *>(heap->alloc.alloc(sizeof(GcObjectEntry) * capacity));
        if (!obj->entries) {
            heap->alloc.free(obj);
            return nullptr;
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            obj->entries[i].key = nullptr;
            obj->entries[i].hash = 0;
            obj->entries[i].next_bucket = -1;
            obj->entries[i].prev_order = -1;
            obj->entries[i].next_order = -1;
            obj->entries[i].next_free = -1;
            obj->entries[i].occupied = false;
            std::construct_at(&obj->entries[i].value);
        }
        obj->bucket_count = bucket_count_for_entries(capacity);
        if (obj->bucket_count == 0) {
            for (std::size_t i = 0; i < capacity; ++i) {
                std::destroy_at(&obj->entries[i].value);
            }
            heap->alloc.free(obj->entries);
            heap->alloc.free(obj);
            return nullptr;
        }
        obj->buckets =
            static_cast<std::int32_t *>(heap->alloc.alloc(sizeof(std::int32_t) * obj->bucket_count));
        if (!obj->buckets) {
            for (std::size_t i = 0; i < capacity; ++i) {
                std::destroy_at(&obj->entries[i].value);
            }
            heap->alloc.free(obj->entries);
            heap->alloc.free(obj);
            return nullptr;
        }
        for (std::size_t i = 0; i < obj->bucket_count; ++i) {
            obj->buckets[i] = -1;
        }
        obj->bucket_mask = obj->bucket_count - 1;
    }
    gc_link(heap, hdr);
    return obj;
}

bool gc_object_reserve(GcHeap *heap, GcObject *obj, std::size_t expected) {
    if (!heap || !obj) {
        return false;
    }
    std::size_t new_capacity = obj->entry_capacity ? obj->entry_capacity : 1;
    if (expected > new_capacity) {
        while (new_capacity < expected) {
            new_capacity *= 2;
        }
        if (!grow_entries(heap, obj, new_capacity)) {
            return false;
        }
    } else if (obj->entry_capacity == 0 && expected > 0) {
        if (!grow_entries(heap, obj, expected)) {
            return false;
        }
    }
    std::size_t desired_bucket_count =
        bucket_count_for_entries(std::max(expected, obj->size));
    if (desired_bucket_count > obj->bucket_count) {
        if (!rehash_buckets(heap, obj, desired_bucket_count)) {
            return false;
        }
    } else if (obj->bucket_count == 0 && desired_bucket_count > 0) {
        if (!rehash_buckets(heap, obj, desired_bucket_count)) {
            return false;
        }
    }
    return true;
}

bool gc_object_set(GcHeap *heap, GcObject *obj, GcString *key, JsValue value) {
    if (!obj || !key) {
        return false;
    }
    std::uint64_t hash = string_hash(key);
    int32_t existing = find_entry_index(obj, key, hash);
    if (existing != -1) {
        obj->entries[existing].value = std::move(value);
        return true;
    }
    if (!gc_object_reserve(heap, obj, obj->size + 1)) {
        return false;
    }
    if (obj->bucket_count == 0 || !obj->buckets || !obj->entries) {
        return false;
    }
    int32_t idx = allocate_entry(obj);
    if (idx == -1) {
        return false;
    }
    GcObjectEntry &entry = obj->entries[idx];
    entry.key = key;
    entry.value = std::move(value);
    entry.hash = hash;
    entry.occupied = true;
    entry.next_free = -1;
    std::size_t bucket = static_cast<std::size_t>(hash) & obj->bucket_mask;
    entry.next_bucket = obj->buckets[bucket];
    obj->buckets[bucket] = idx;
    entry.prev_order = obj->tail;
    entry.next_order = -1;
    if (obj->tail != -1) {
        obj->entries[obj->tail].next_order = idx;
    } else {
        obj->head = idx;
    }
    obj->tail = idx;
    obj->size += 1;
    return true;
}

const JsValue *gc_object_get(const GcObject *obj, const GcString *key) {
    if (!obj || !key) {
        return nullptr;
    }
    std::uint64_t hash = string_hash(key);
    int32_t idx = find_entry_index(obj, key, hash);
    if (idx == -1) {
        return nullptr;
    }
    return &obj->entries[idx].value;
}

bool gc_object_remove(GcObject *obj, const GcString *key) {
    if (!obj || !key || obj->bucket_count == 0 || !obj->buckets) {
        return false;
    }
    std::uint64_t hash = string_hash(key);
    std::size_t bucket = static_cast<std::size_t>(hash) & obj->bucket_mask;
    int32_t prev = -1;
    int32_t idx = obj->buckets[bucket];
    while (idx != -1) {
        GcObjectEntry &entry = obj->entries[idx];
        if (entry.occupied && entry.hash == hash && string_equals(entry.key, key)) {
            if (prev == -1) {
                obj->buckets[bucket] = entry.next_bucket;
            } else {
                obj->entries[prev].next_bucket = entry.next_bucket;
            }
            if (entry.prev_order != -1) {
                obj->entries[entry.prev_order].next_order = entry.next_order;
            } else {
                obj->head = entry.next_order;
            }
            if (entry.next_order != -1) {
                obj->entries[entry.next_order].prev_order = entry.prev_order;
            } else {
                obj->tail = entry.prev_order;
            }
            entry.occupied = false;
            entry.key = nullptr;
            entry.hash = 0;
            entry.next_bucket = -1;
            entry.prev_order = -1;
            entry.next_order = -1;
            entry.value = JsValue();
            entry.next_free = obj->free_head;
            obj->free_head = idx;
            obj->size -= 1;
            return true;
        }
        prev = idx;
        idx = entry.next_bucket;
    }
    return false;
}

const GcObjectEntry *gc_object_entry_at(const GcObject *obj, std::size_t index) {
    if (!obj || index >= obj->size) {
        return nullptr;
    }
    std::size_t current = 0;
    int32_t cursor = obj->head;
    while (cursor != -1) {
        if (current == index) {
            return &obj->entries[cursor];
        }
        cursor = obj->entries[cursor].next_order;
        current += 1;
    }
    return nullptr;
}

void gc_collect(GcHeap *heap, JsValue **roots, std::size_t root_count) {
    heap->live_mark = flip_mark(heap->live_mark);
    for (std::size_t i = 0; i < root_count; ++i) {
        gc_mark_value(heap, *roots[i]);
    }
    GcHeader **cursor = &heap->head;
    while (*cursor) {
        GcHeader *obj = *cursor;
        if (obj->mark_ != heap->live_mark) {
            *cursor = obj->next;
            gc_free_obj(heap, obj);
        } else {
            cursor = &obj->next;
        }
    }
}

void GcRootSet::add_global(JsValue *value) {
    if (value) {
        globals_.push_back(value);
    }
}

void GcRootSet::remove_global(JsValue *value) {
    for (std::size_t i = 0; i < globals_.size(); ++i) {
        if (globals_[i] == value) {
            globals_[i] = globals_.back();
            globals_.pop_back();
            return;
        }
    }
}

void GcRootSet::push_frame() {
    frames_.push_back(stack_.size());
}

void GcRootSet::pop_frame() {
    if (frames_.empty()) {
        return;
    }
    std::size_t size = frames_.back();
    frames_.pop_back();
    if (size < stack_.size()) {
        stack_.resize(size);
    }
}

void GcRootSet::add_stack_root(JsValue *value) {
    if (value) {
        stack_.push_back(value);
    }
}

void GcRootSet::add_temp_root(JsValue *value) {
    if (value) {
        temps_.push_back(value);
    }
}

void GcRootSet::remove_temp_root(JsValue *value) {
    for (std::size_t i = 0; i < temps_.size(); ++i) {
        if (temps_[i] == value) {
            temps_[i] = temps_.back();
            temps_.pop_back();
            return;
        }
    }
}

void GcRootSet::collect(GcHeap &heap) {
    std::vector<JsValue *> roots;
    roots.reserve(globals_.size() + stack_.size() + temps_.size());
    roots.insert(roots.end(), globals_.begin(), globals_.end());
    roots.insert(roots.end(), stack_.begin(), stack_.end());
    roots.insert(roots.end(), temps_.begin(), temps_.end());
    gc_collect(&heap, roots.data(), roots.size());
}

GcRootHandle::GcRootHandle(GcRootSet &roots, JsValue *value)
    : roots_(&roots), value_(value) {
    if (roots_ && value_) {
        roots_->add_temp_root(value_);
    }
}

GcRootHandle::GcRootHandle(GcRootHandle &&other) noexcept
    : roots_(other.roots_), value_(other.value_) {
    other.roots_ = nullptr;
    other.value_ = nullptr;
}

GcRootHandle &GcRootHandle::operator=(GcRootHandle &&other) noexcept {
    if (this != &other) {
        reset();
        roots_ = other.roots_;
        value_ = other.value_;
        other.roots_ = nullptr;
        other.value_ = nullptr;
    }
    return *this;
}

GcRootHandle::~GcRootHandle() {
    reset();
}

void GcRootHandle::reset() {
    if (roots_ && value_) {
        roots_->remove_temp_root(value_);
    }
    roots_ = nullptr;
    value_ = nullptr;
}

} // namespace fiber::json
