//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSGC_H
#define FIBER_JSGC_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "../common/mem/Allocator.h"
#include "../common/mem/BufPool.h"
#include "GcRootSet.h"
#include "JsValue.h"

namespace fiber::script {

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
    bool first_collect_protected = false;
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
    JsValue current_key;
    JsValue current_value;
    bool has_current = false;
};

class ConstValueHandle {
public:
    constexpr ConstValueHandle() noexcept = default;
    constexpr ConstValueHandle(std::nullptr_t) noexcept {}
    constexpr ConstValueHandle(fiber::script::JsValue *value) noexcept : value_(value) {}

    [[nodiscard]] const fiber::script::JsValue *get() const noexcept { return value_; }
    [[nodiscard]] const fiber::script::JsValue &operator*() const noexcept { return *value_; }
    [[nodiscard]] const fiber::script::JsValue *operator->() const noexcept { return value_; }
    [[nodiscard]] const fiber::script::JsValue &operator[](std::size_t index) const noexcept { return value_[index]; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    fiber::script::JsValue *value_ = nullptr;
};

class ValueHandle {
public:
    constexpr ValueHandle() noexcept = default;
    constexpr ValueHandle(std::nullptr_t) noexcept {}
    constexpr ValueHandle(fiber::script::JsValue *value) noexcept : value_(value) {}

    [[nodiscard]] fiber::script::JsValue *get() const noexcept { return value_; }
    [[nodiscard]] fiber::script::JsValue &operator*() const noexcept { return *value_; }
    [[nodiscard]] fiber::script::JsValue *operator->() const noexcept { return value_; }
    [[nodiscard]] fiber::script::JsValue &operator[](std::size_t index) const noexcept { return value_[index]; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] ConstValueHandle as_constant() const noexcept { return ConstValueHandle(value_); }
    [[nodiscard]] operator ConstValueHandle() const noexcept { return as_constant(); }

private:
    fiber::script::JsValue *value_ = nullptr;
};

static_assert(sizeof(ConstValueHandle) == sizeof(fiber::script::JsValue *));
static_assert(sizeof(ValueHandle) == sizeof(fiber::script::JsValue *));

constexpr bool operator==(ConstValueHandle handle, std::nullptr_t) noexcept { return !handle; }
constexpr bool operator==(std::nullptr_t, ConstValueHandle handle) noexcept { return !handle; }
constexpr bool operator!=(ConstValueHandle handle, std::nullptr_t) noexcept { return static_cast<bool>(handle); }
constexpr bool operator!=(std::nullptr_t, ConstValueHandle handle) noexcept { return static_cast<bool>(handle); }
constexpr bool operator==(ValueHandle handle, std::nullptr_t) noexcept { return !handle; }
constexpr bool operator==(std::nullptr_t, ValueHandle handle) noexcept { return !handle; }
constexpr bool operator!=(ValueHandle handle, std::nullptr_t) noexcept { return static_cast<bool>(handle); }
constexpr bool operator!=(std::nullptr_t, ValueHandle handle) noexcept { return static_cast<bool>(handle); }

struct GcCollectStats {
    std::size_t total = 0;
    std::size_t freed = 0;
};

class GcHeap final : public GcRootSource {
public:
    class LocalMark;

    GcHeap();
    explicit GcHeap(fiber::mem::BufPool &pool);
    ~GcHeap() override;
    GcHeap(const GcHeap &) = delete;
    GcHeap &operator=(const GcHeap &) = delete;
    GcHeap(GcHeap &&) = delete;
    GcHeap &operator=(GcHeap &&) = delete;

    [[nodiscard]] GcHeap &heap() noexcept { return *this; }
    [[nodiscard]] const GcHeap &heap() const noexcept { return *this; }
    [[nodiscard]] GcRootSet &roots() noexcept { return roots_; }
    [[nodiscard]] const GcRootSet &roots() const noexcept { return roots_; }

    [[nodiscard]] ValueHandle local_value();
    [[nodiscard]] ValueHandle global_value();

    void visit_roots(fiber::script::GcRootVisitor &visitor) noexcept override;

    GcCollectStats collect();

    GcHeader *head = nullptr;
    std::size_t bytes = 0;
    std::size_t threshold = 1 << 20;
    GcMark live_mark = GcMark::GcMark_0;
    mem::Allocator alloc;

private:
    struct ValueBlock {
        ValueBlock *next = nullptr;
        fiber::script::JsValue slots[8];
    };

    struct LocalState {
        ValueBlock *block = nullptr;
        fiber::script::JsValue *top = nullptr;
    };

    friend class LocalMark;

    [[nodiscard]] LocalState mark_local() const noexcept;
    void restore_local(LocalState state) noexcept;
    [[nodiscard]] ValueBlock *alloc_value_block();
    [[nodiscard]] ValueBlock *acquire_local_block();
    [[nodiscard]] ValueBlock *acquire_global_block();
    static void reset_block(ValueBlock *block) noexcept;
    void recycle_local_blocks(ValueBlock *first) noexcept;

    fiber::script::GcRootSet roots_;
    fiber::mem::BufPool owned_pool_;
    fiber::mem::BufPool *pool_ = nullptr;

    ValueBlock *local_head_ = nullptr;
    ValueBlock *local_current_ = nullptr;
    ValueBlock *local_free_ = nullptr;
    fiber::script::JsValue *local_top_ = nullptr;
    fiber::script::JsValue *local_end_ = nullptr;

    ValueBlock *global_head_ = nullptr;
    ValueBlock *global_current_ = nullptr;
    fiber::script::JsValue *global_top_ = nullptr;
    fiber::script::JsValue *global_end_ = nullptr;
};

class GcHeap::LocalMark {
public:
    explicit LocalMark(GcHeap &heap) noexcept;
    LocalMark(const LocalMark &) = delete;
    LocalMark &operator=(const LocalMark &) = delete;
    LocalMark(LocalMark &&other) noexcept;
    LocalMark &operator=(LocalMark &&other) noexcept;
    ~LocalMark();

    void reset() noexcept;

private:
    GcHeap *heap_ = nullptr;
    LocalState state_{};
};

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len) noexcept;
GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) noexcept;
GcString *gc_new_string_bytes_uninit(GcHeap *heap, std::size_t len) noexcept;
GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len) noexcept;
GcString *gc_new_string_utf16_uninit(GcHeap *heap, std::size_t len) noexcept;
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

} // namespace fiber::script

#endif // FIBER_JSGC_H
