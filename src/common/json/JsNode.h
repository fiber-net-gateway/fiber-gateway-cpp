//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSNODE_H
#define FIBER_JSNODE_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fiber::json {

enum class GcMark : std::uint8_t {
    GcMark_0,
    GcMark_1,
};

struct GcHeader;
struct GcHeap;

enum class JsNodeType : std::uint8_t {
    Undefined = 0,
    Null,
    Boolean,
    Integer,
    Float,
    HeapString,
    NativeString,
    Array,
    Object,
    Interator,
    Exception,
    NativeBinary,
    HeapBinary,
};

enum class JsTag : std::uint8_t {
    Undefined = 0,
    Null,
    Boolean,
    Int64,
    Double,
    HeapRef,
    BorrowedString,
    BorrowedBinary,
};

enum class JsHeapKind : std::uint8_t {
    String = 0,
    Binary,
    Array,
    Object,
    Exception,
    Iterator,
};

enum class JsBorrowedEncoding : std::uint8_t {
    Utf8 = 0,
    Byte,
};

struct NativeStr {
public:
    std::size_t len = 0;
    const char *data = nullptr;
};

struct NativeBin {
public:
    std::size_t len = 0;
    const std::uint8_t *data = nullptr;
};

struct alignas(16) JsValue {
    static JsValue make_undefined();
    static JsValue make_null();
    static JsValue make_boolean(bool value);
    static JsValue make_integer(int64_t value);
    static JsValue make_float(double value);
    static JsValue make_native_string(const char *data, std::size_t len);
    static JsValue make_native_binary(const std::uint8_t *data, std::size_t len);
    static JsValue make_string(GcHeap &heap, const char *data, std::size_t len);
    static JsValue make_binary(GcHeap &heap, const std::uint8_t *data, std::size_t len);
    static JsValue make_array(GcHeap &heap, std::size_t capacity);
    static JsValue make_object(GcHeap &heap, std::size_t capacity);

    constexpr JsValue() = default;
    JsValue(const JsValue &) = default;
    JsValue(JsValue &&) noexcept = default;
    JsValue &operator=(const JsValue &) = default;
    JsValue &operator=(JsValue &&) noexcept = default;
    ~JsValue() = default;

    std::uint64_t payload = 0;
    std::uint32_t aux32 = 0;
    std::uint16_t aux16 = 0;
    std::uint8_t tag = static_cast<std::uint8_t>(JsTag::Undefined);
    std::uint8_t subtag = 0;
};

JsValue js_make_heap_ref(GcHeader *hdr, JsHeapKind kind);
JsValue js_make_borrowed_string(const char *data, std::size_t len, JsBorrowedEncoding encoding);
JsValue js_make_borrowed_binary(const std::uint8_t *data, std::size_t len);

JsTag js_value_tag(const JsValue &value);
JsNodeType js_value_type(const JsValue &value);
bool js_value_is_heap_ref(const JsValue &value);
bool js_value_is_borrowed_string(const JsValue &value);
bool js_value_is_borrowed_binary(const JsValue &value);
bool js_value_is_undefined(const JsValue &value);

bool js_value_bool(const JsValue &value);
std::int64_t js_value_int64(const JsValue &value);
double js_value_double(const JsValue &value);
NativeStr js_value_native_string(const JsValue &value);
NativeBin js_value_native_binary(const JsValue &value);

GcHeader *js_value_heap_header(JsValue &value);
const GcHeader *js_value_heap_header(const JsValue &value);

template <typename T>
T *js_value_heap_ptr(JsValue &value) {
    return reinterpret_cast<T *>(js_value_heap_header(value));
}

template <typename T>
const T *js_value_heap_ptr(const JsValue &value) {
    return reinterpret_cast<const T *>(js_value_heap_header(value));
}

static_assert(std::is_trivially_copyable_v<NativeStr>);
static_assert(std::is_trivially_copyable_v<NativeBin>);
static_assert(sizeof(JsValue) == 16);
static_assert(std::is_trivially_copyable_v<JsValue>);

} // namespace fiber::json

#endif // FIBER_JSNODE_H
