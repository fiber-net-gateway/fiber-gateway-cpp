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

struct GcHeader;
struct GcString;

enum class GcIterStep : std::uint8_t {
    Item,
    Done,
    Mutated,
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
    class NoGcScope;

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
    void maybe_collect_for_alloc(std::size_t bytes);
    [[nodiscard]] bool no_gc_active() const noexcept { return no_gc_depth_ != 0; }

    GcHeader *head = nullptr;
    std::size_t bytes = 0;
    std::size_t threshold = 1 << 20;
    GcMark live_mark = GcMark::GcMark_0;
    mem::Allocator alloc;
    GcString **string_intern_buckets = nullptr;
    std::size_t string_intern_bucket_count = 0;
    std::size_t string_intern_size = 0;

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
    void enter_no_gc() noexcept;
    void leave_no_gc() noexcept;
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

    std::uint32_t no_gc_depth_ = 0;
    bool gc_pending_ = false;
    bool collecting_ = false;
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

class GcHeap::NoGcScope {
public:
    explicit NoGcScope(GcHeap &heap) noexcept;
    NoGcScope(const NoGcScope &) = delete;
    NoGcScope &operator=(const NoGcScope &) = delete;
    NoGcScope(NoGcScope &&other) noexcept;
    NoGcScope &operator=(NoGcScope &&other) noexcept;
    ~NoGcScope();

    void reset() noexcept;

private:
    GcHeap *heap_ = nullptr;
};

using GcByteStringWriter = bool (*)(std::uint8_t *dst, std::size_t len, void *ctx) noexcept;
using GcUtf16StringWriter = bool (*)(char16_t *dst, std::size_t len, void *ctx) noexcept;

bool gc_make_string(GcHeap *heap, ValueHandle out, const char *data, std::size_t len) noexcept;
bool gc_make_string_bytes(GcHeap *heap, ValueHandle out, const std::uint8_t *data, std::size_t len) noexcept;
bool gc_make_string_bytes_uninit(GcHeap *heap, ValueHandle out, std::size_t len, GcByteStringWriter writer,
                                 void *ctx) noexcept;
bool gc_make_string_utf16(GcHeap *heap, ValueHandle out, const char16_t *data, std::size_t len) noexcept;
bool gc_make_string_utf16_uninit(GcHeap *heap, ValueHandle out, std::size_t len, GcUtf16StringWriter writer,
                                 void *ctx) noexcept;
bool gc_make_binary(GcHeap *heap, ValueHandle out, const std::uint8_t *data, std::size_t len);
bool gc_make_array(GcHeap *heap, ValueHandle out, std::size_t capacity);
bool gc_make_object(GcHeap *heap, ValueHandle out, std::size_t capacity);
bool gc_make_exception(GcHeap *heap, ValueHandle out, std::int64_t position, const char *name, std::size_t name_len,
                       const char *message, std::size_t message_len, JsValue meta);
bool gc_make_exception(GcHeap *heap, ValueHandle out, std::int64_t position, const char *name, std::size_t name_len,
                       const char *message, std::size_t message_len);
bool gc_make_empty_iterator(GcHeap *heap, ValueHandle out);
bool gc_make_array_iterator(GcHeap *heap, ValueHandle out, ConstValueHandle array);
bool gc_make_object_iterator(GcHeap *heap, ValueHandle out, ConstValueHandle object);

bool gc_string_to_utf8(ConstValueHandle value, std::string &out);
bool gc_array_get(ConstValueHandle array, std::size_t index, ValueHandle out);
bool gc_array_set(GcHeap *heap, ValueHandle array, std::size_t index, JsValue value);
bool gc_array_push(GcHeap *heap, ValueHandle array, JsValue value);
bool gc_array_pop(ValueHandle array, ValueHandle out);
bool gc_array_insert(GcHeap *heap, ValueHandle array, std::size_t index, JsValue value);
bool gc_array_remove(ValueHandle array, std::size_t index, ValueHandle out);
bool gc_object_set(GcHeap *heap, ValueHandle object, JsValue key, JsValue value);
bool gc_object_set_key(GcHeap *heap, ValueHandle object, const char *key, std::size_t key_len, JsValue value);
bool gc_object_get(GcHeap *heap, ConstValueHandle object, JsValue key, ValueHandle out);
bool gc_object_get_key(GcHeap *heap, ConstValueHandle object, const char *key, std::size_t key_len, ValueHandle out);
bool gc_object_remove(GcHeap *heap, ValueHandle object, JsValue key);
bool gc_object_remove_key(GcHeap *heap, ValueHandle object, const char *key, std::size_t key_len);
GcIterStep gc_iterator_next(GcHeap *heap, ValueHandle iter);

} // namespace fiber::script

#endif // FIBER_JSGC_H
