//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSNODE_H
#define FIBER_JSNODE_H

#include <cstddef>
#include <cstdint>

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

struct NativeStr {
public:
    std::size_t len = 0;
    char *data = nullptr;
};

struct NativeBin {
public:
    std::size_t len = 0;
    std::uint8_t *data = nullptr;
};

struct JsValue {
    static JsValue make_undefined();
    static JsValue make_null();
    static JsValue make_integer(int64_t value);
    static JsValue make_float(double value);
    static JsValue make_native_string(char *data, std::size_t len);
    static JsValue make_native_binary(std::uint8_t *data, std::size_t len);
    static JsValue make_string(GcHeap &heap, const char *data, std::size_t len);
    static JsValue make_binary(GcHeap &heap, const std::uint8_t *data, std::size_t len);
    static JsValue make_array(GcHeap &heap, std::size_t capacity);
    static JsValue make_object(GcHeap &heap, std::size_t capacity);

    JsValue();
    JsValue(const JsValue &other);
    JsValue(JsValue &&other) noexcept;
    JsValue &operator=(const JsValue &other);
    JsValue &operator=(JsValue &&other) noexcept;
    ~JsValue();

    JsNodeType type_;
    union {
        int64_t i;
        double f;
        GcHeader *gc;
        NativeStr ns;
        NativeBin nb;
    };

private:
    void destroy();
    void copy_from(const JsValue &other);
    void move_from(JsValue &&other);
};

} // namespace fiber::json


#endif // FIBER_JSNODE_H
