//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSGC_H
#define FIBER_JSGC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "JsNode.h"
#include "../mem/Allocator.h"

namespace fiber::json {

enum class GcKind : std::uint8_t {
    String,
    Binary,
    Array,
    Object,
    Exception,
    Iterator,
};

struct GcHeader {
    GcHeader *next = nullptr;
    GcMark mark_ = GcMark::GcMark_0;
    GcKind kind = GcKind::String;
    std::uint32_t size_ = 0;
};

enum class GcStringEncoding : std::uint8_t {
    Byte,
    Utf16,
};

struct GcString {
    GcHeader hdr;
    std::size_t len = 0;
    std::uint64_t hash = 0;
    bool hash_valid = false;
    GcStringEncoding encoding = GcStringEncoding::Byte;
    union {
        std::uint8_t *data8;
        char16_t *data16;
    };
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

enum class GcIteratorMode : std::uint8_t {
    Keys,
    Values,
    Entries,
};

struct GcIterator {
    GcHeader hdr;
    GcIteratorKind kind = GcIteratorKind::Array;
    GcIteratorMode mode = GcIteratorMode::Values;
    std::uint64_t expected_version = 0;
    bool using_snapshot = false;
    GcArray *array = nullptr;
    GcObject *object = nullptr;
    std::size_t index = 0;
    std::int32_t cursor = -1;
    GcString **snapshot_keys = nullptr;
    std::size_t snapshot_size = 0;
    std::size_t snapshot_index = 0;
};

struct GcHeap {
    GcHeader *head = nullptr;
    std::size_t bytes = 0;
    std::size_t threshold = 1 << 20;
    GcMark live_mark = GcMark::GcMark_0;
    mem::Allocator alloc;
};

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len);
GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len);
GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len);
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
GcIterator *gc_new_array_iterator(GcHeap *heap, GcArray *array, GcIteratorMode mode);
GcIterator *gc_new_object_iterator(GcHeap *heap, GcObject *object, GcIteratorMode mode);
bool gc_iterator_next(GcHeap *heap, GcIterator *iter, JsValue &out, bool &done);
bool gc_object_reserve(GcHeap *heap, GcObject *obj, std::size_t expected);
bool gc_object_set(GcHeap *heap, GcObject *obj, GcString *key, JsValue value);
const JsValue *gc_object_get(const GcObject *obj, const GcString *key);
bool gc_object_remove(GcObject *obj, const GcString *key);
const GcObjectEntry *gc_object_entry_at(const GcObject *obj, std::size_t index);
void gc_collect(GcHeap *heap, JsValue **roots, std::size_t root_count);

class GcRootSet {
public:
    void add_global(JsValue *value);
    void remove_global(JsValue *value);

    void push_frame();
    void pop_frame();
    void add_stack_root(JsValue *value);

    void add_temp_root(JsValue *value);
    void remove_temp_root(JsValue *value);

    void collect(GcHeap &heap);

private:
    std::vector<JsValue *> globals_;
    std::vector<JsValue *> stack_;
    std::vector<std::size_t> frames_;
    std::vector<JsValue *> temps_;
};

class GcRootHandle {
public:
    GcRootHandle() = default;
    GcRootHandle(GcRootSet &roots, JsValue *value);
    GcRootHandle(const GcRootHandle &) = delete;
    GcRootHandle &operator=(const GcRootHandle &) = delete;
    GcRootHandle(GcRootHandle &&other) noexcept;
    GcRootHandle &operator=(GcRootHandle &&other) noexcept;
    ~GcRootHandle();

    void reset();

private:
    GcRootSet *roots_ = nullptr;
    JsValue *value_ = nullptr;
};

} // namespace fiber::json

#endif // FIBER_JSGC_H
