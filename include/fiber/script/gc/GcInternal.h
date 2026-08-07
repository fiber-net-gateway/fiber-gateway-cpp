//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_SCRIPT_GC_INTERNAL_H
#define FIBER_SCRIPT_GC_INTERNAL_H

#include "../JsGc.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::script {

struct GcHeader {
    GcHeader *next = nullptr;
    GcMark mark_ = GcMark::GcMark_0;
    GcHeapKind kind = GcHeapKind::String;
    bool first_collect_protected = false;
    std::uint32_t size_ = 0;
};

inline constexpr std::uint8_t kGcStringHashValid = 1u << 0;
inline constexpr std::uint8_t kGcStringWellFormed = 1u << 1;

// Canonical WTF-8 bytes plus a trailing NUL follow the fixed header. hdr.size_
// covers both regions, so the byte length is derivable without a data pointer.
struct GcString {
    GcHeader hdr;
    GcString *intern_next = nullptr;
    std::uint64_t hash = 0;
    std::uint32_t utf16_len = 0;
    std::uint8_t flags = 0;
};

static_assert(sizeof(void *) != 8 || sizeof(GcString) == 40);

inline char *gc_string_wtf8_data(GcString *str) noexcept { return reinterpret_cast<char *>(str) + sizeof(GcString); }

inline const char *gc_string_wtf8_data(const GcString *str) noexcept {
    return reinterpret_cast<const char *>(str) + sizeof(GcString);
}

inline std::size_t gc_string_byte_len(const GcString *str) noexcept {
    constexpr std::size_t overhead = sizeof(GcString) + 1;
    return str && str->hdr.size_ >= overhead ? static_cast<std::size_t>(str->hdr.size_) - overhead : 0;
}

inline bool gc_string_is_ascii(const GcString *str) noexcept {
    return str && gc_string_byte_len(str) == str->utf16_len;
}

inline bool gc_string_is_well_formed(const GcString *str) noexcept {
    return str && (str->flags & kGcStringWellFormed) != 0;
}

struct GcStringUtf8Buffer {
    char *ptr = nullptr;
    std::size_t capacity = 0;
};

enum class GcStringUtf8Boundary : std::uint8_t {
    PreserveCodePoint,
    AllowSplitCodePoint,
};

enum class GcStringUtf8Status : std::uint8_t {
    Done,
    NeedMore,
    Invalid,
};

struct GcStringUtf8Cursor {
    std::size_t index = 0;
};

struct GcStringUtf8Result {
    GcStringUtf8Status status = GcStringUtf8Status::Done;
    std::size_t written = 0;
    std::size_t needed = 0;
};

struct GcBinary {
    GcHeader hdr;
    std::size_t len = 0;
    std::uint8_t *data = nullptr;
};

struct GcArray {
    GcHeader hdr;
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::uint64_t version = 0;
    JsValue *elems = nullptr;
};

struct GcObjectEntry {
    GcString *key = nullptr;
    JsValue value;
    std::uint64_t hash = 0;
    std::int32_t next_bucket = -1;
    std::int32_t prev_order = -1;
    std::int32_t next_order = -1;
    std::int32_t next_free = -1;
    bool occupied = false;
};

struct GcObject {
    GcHeader hdr;
    std::size_t size = 0;
    std::uint64_t version = 0;
    std::size_t entry_count = 0;
    std::size_t entry_capacity = 0;
    std::size_t bucket_count = 0;
    std::size_t bucket_mask = 0;
    std::int32_t head = -1;
    std::int32_t tail = -1;
    std::int32_t free_head = -1;
    std::int32_t *buckets = nullptr;
    GcObjectEntry *entries = nullptr;
};

struct GcException {
    GcHeader hdr;
    std::int64_t position = -1;
    GcString *name = nullptr;
    GcString *message = nullptr;
    JsValue meta;
};

enum class GcIteratorKind : std::uint8_t {
    Array,
    Object,
};

struct GcIterator {
    GcHeader hdr;
    GcIteratorKind kind = GcIteratorKind::Array;
    bool has_current = false;
    std::uint64_t expected_version = 0;
    JsValue current_key{};
    JsValue current_value{};
    union {
        GcArray *array;
        GcObject *object;
    };
    union {
        std::size_t index;
        std::int32_t cursor;
    };
};

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len) noexcept;
GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) noexcept;
GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len) noexcept;
GcString *gc_new_string_wtf8_uninit(GcHeap *heap, std::size_t byte_len, std::size_t utf16_len,
                                    bool well_formed) noexcept;
GcString *gc_new_string_substring_utf16(GcHeap *heap, const GcString *source, std::size_t begin,
                                        std::size_t end) noexcept;
bool gc_string_can_encode_utf8(const GcString *str) noexcept;
bool gc_string_utf8_view(const GcString *str, std::string_view &out) noexcept;
GcStringUtf8Result gc_string_to_utf8(const GcString *str, GcStringUtf8Cursor &cursor, GcStringUtf8Buffer out,
                                     GcStringUtf8Boundary boundary) noexcept;
bool gc_string_to_utf8(const GcString *str, std::string &out);
GcBinary *gc_new_binary(GcHeap *heap, const std::uint8_t *data, std::size_t len);
GcArray *gc_new_array(GcHeap *heap, std::size_t capacity);
bool gc_array_reserve(GcHeap *heap, GcArray *arr, std::size_t expected);
const JsValue *gc_array_get(const GcArray *arr, std::size_t index);
bool gc_array_set(GcHeap *heap, GcArray *arr, std::size_t index, JsValue value);
bool gc_array_push(GcHeap *heap, GcArray *arr, JsValue value);
bool gc_array_pop(GcArray *arr, JsValue *out);
bool gc_array_insert(GcHeap *heap, GcArray *arr, std::size_t index, JsValue value);
bool gc_array_remove(GcArray *arr, std::size_t index, JsValue *out);
GcObject *gc_new_object(GcHeap *heap, std::size_t capacity);
GcException *gc_new_exception(GcHeap *heap, std::int64_t position, GcString *name, GcString *message, JsValue meta);
GcException *gc_new_exception(GcHeap *heap, std::int64_t position, GcString *name, GcString *message);
GcException *gc_new_exception(GcHeap *heap, std::int64_t position, const char *name, std::size_t name_len,
                              const char *message, std::size_t message_len, JsValue meta);
GcException *gc_new_exception(GcHeap *heap, std::int64_t position, const char *name, std::size_t name_len,
                              const char *message, std::size_t message_len);
GcIterator *gc_new_array_iterator(GcHeap *heap, GcArray *array);
GcIterator *gc_new_object_iterator(GcHeap *heap, GcObject *object);
GcIterStep gc_iterator_next(GcHeap *heap, GcIterator *iter);
bool gc_object_reserve(GcHeap *heap, GcObject *obj, std::size_t expected);
bool gc_object_set(GcHeap *heap, GcObject *obj, GcString *key, JsValue value);
bool gc_object_set_heap_key(GcHeap *heap, ValueHandle object, const GcString *key, JsValue value);
const JsValue *gc_object_get(const GcObject *obj, const GcString *key);
bool gc_object_remove(GcObject *obj, const GcString *key);
const GcObjectEntry *gc_object_first_entry(const GcObject *obj);
const GcObjectEntry *gc_object_next_entry(const GcObject *obj, const GcObjectEntry *entry);

} // namespace fiber::script

namespace fiber::script::gc_detail {

inline constexpr std::size_t kValueBlockSlots = 8;
inline constexpr std::size_t kMaxInternStringLen = 64;

std::size_t saturating_add(std::size_t lhs, std::size_t rhs);
std::size_t next_threshold(std::size_t live_bytes);
GcMark flip_mark(GcMark mark);

std::size_t array_storage_bytes(std::size_t capacity);
std::size_t entry_storage_bytes(std::size_t capacity);
std::size_t bucket_storage_bytes(std::size_t bucket_count);
std::size_t bucket_count_for_entries(std::size_t entry_capacity);

void gc_account_add(GcHeap *heap, std::size_t bytes);
void gc_account_sub(GcHeap *heap, std::size_t bytes);
void *gc_alloc_extra(GcHeap *heap, std::size_t bytes);
void gc_free_extra(GcHeap *heap, void *ptr, std::size_t bytes);

std::uint64_t string_hash(const GcString *str);
std::uint64_t string_hash_wtf8(const char *data, std::size_t len) noexcept;
bool string_equals(const GcString *lhs, const GcString *rhs);
GcString *gc_string_intern_lookup_wtf8(GcHeap *heap, const char *data, std::size_t byte_len, std::size_t utf16_len,
                                       std::uint64_t hash) noexcept;
void gc_string_intern_insert(GcHeap *heap, GcString *str, std::uint64_t hash) noexcept;
GcString *gc_string_intern_final(GcHeap *heap, GcString *str) noexcept;
void gc_string_intern_remove(GcHeap *heap, GcString *str) noexcept;
void gc_string_intern_free_table(GcHeap *heap) noexcept;
std::int32_t find_entry_index(const GcObject *obj, const GcString *key, std::uint64_t hash);
bool rehash_buckets(GcHeap *heap, GcObject *obj, std::size_t new_bucket_count);
bool grow_entries(GcHeap *heap, GcObject *obj, std::size_t new_capacity);
std::int32_t allocate_entry(GcObject *obj);

JsValue make_heap_string_value(GcString *str);

GcHeader *gc_alloc_raw(GcHeap *heap, std::size_t size, GcHeapKind kind);
void gc_link(GcHeap *heap, GcHeader *hdr);
void gc_mark_value(GcHeap *heap, const JsValue &value);
void gc_mark_obj(GcHeap *heap, GcHeader *obj);
void gc_free_obj(GcHeap *heap, GcHeader *obj);
void gc_sweep_unmarked(GcHeap *heap);

} // namespace fiber::script::gc_detail

#endif // FIBER_SCRIPT_GC_INTERNAL_H
