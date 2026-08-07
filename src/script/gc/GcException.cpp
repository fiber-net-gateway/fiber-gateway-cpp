//
// Created by dear on 2025/12/30.
//

#include <fiber/script/gc/GcInternal.h>

#include <fiber/common/Assert.h>

#include <new>

namespace fiber::script {

using namespace gc_detail;

namespace {

GcException *gc_new_exception_unchecked(GcHeap *heap, std::int64_t position, GcString *name, GcString *message,
                                        const JsValue &meta) {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcException), GcHeapKind::Exception);
    if (!hdr) {
        return nullptr;
    }
    auto *exc = reinterpret_cast<GcException *>(hdr);
    exc->position = position;
    exc->name = name;
    exc->message = message;
    std::construct_at(&exc->meta);
    exc->meta = meta;
    gc_link(heap, hdr);
    return exc;
}

} // namespace

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, GcString *name, GcString *message, JsValue meta) {
    FIBER_ASSERT(heap->no_gc_active());
    return gc_new_exception_unchecked(heap, position, name, message, meta);
}

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, GcString *name, GcString *message) {
    return gc_new_exception(heap, position, name, message, JsValue::make_undefined());
}

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, const char *name, std::size_t name_len,
                              const char *message, std::size_t message_len, JsValue meta) {
    FIBER_ASSERT(heap->no_gc_active());

    GcString *name_str = nullptr;
    GcString *message_str = nullptr;
    if (name || name_len > 0) {
        if (!name && name_len > 0) {
            return nullptr;
        }
        name_str = gc_new_string(heap, name ? name : "", name_len);
        if (!name_str) {
            return nullptr;
        }
    }
    if (message || message_len > 0) {
        if (!message && message_len > 0) {
            return nullptr;
        }
        message_str = gc_new_string(heap, message ? message : "", message_len);
        if (!message_str) {
            return nullptr;
        }
    }
    return gc_new_exception_unchecked(heap, position, name_str, message_str, meta);
}

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, const char *name, std::size_t name_len,
                              const char *message, std::size_t message_len) {
    return gc_new_exception(heap, position, name, name_len, message, message_len, JsValue::make_undefined());
}

} // namespace fiber::script
