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
    JsValue *elems = nullptr;
};

struct GcObjectEntry {
    GcString *key = nullptr;
    JsValue value;
};

struct GcObject {
    GcHeader hdr;
    std::size_t size = 0;
    std::size_t capacity = 0;
    GcObjectEntry *entries = nullptr;
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
GcObject *gc_new_object(GcHeap *heap, std::size_t capacity);
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
