//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include "../../common/Assert.h"

#include <new>

namespace fiber::script {

using namespace gc_detail;

namespace {

GcException *gc_new_exception_unchecked(GcHeap *heap, std::int64_t position, GcString *name, GcString *message,
                                        const JsValue &meta) {
    if (!heap) {
        return nullptr;
    }
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
    if (!heap) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    GcHeap::LocalMark mark(*heap);
    ValueHandle name_root = heap->local_value();
    ValueHandle message_root = heap->local_value();
    ValueHandle meta_root = heap->local_value();
    if (!name_root || !message_root || !meta_root) {
        return nullptr;
    }
    *name_root = name ? js_make_heap_ref(&name->hdr, GcHeapKind::String) : JsValue::make_undefined();
    *message_root = message ? js_make_heap_ref(&message->hdr, GcHeapKind::String) : JsValue::make_undefined();
    *meta_root = meta;
    return gc_new_exception_unchecked(heap, position, name, message, *meta_root);
}

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, GcString *name, GcString *message) {
    return gc_new_exception(heap, position, name, message, JsValue::make_undefined());
}

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, const char *name, std::size_t name_len,
                              const char *message, std::size_t message_len, JsValue meta) {
    if (!heap) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    GcHeap::LocalMark mark(*heap);
    ValueHandle name_root = heap->local_value();
    ValueHandle message_root = heap->local_value();
    ValueHandle meta_root = heap->local_value();
    if (!name_root || !message_root || !meta_root) {
        return nullptr;
    }
    *name_root = JsValue::make_undefined();
    *message_root = JsValue::make_undefined();
    *meta_root = meta;

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
        *name_root = js_make_heap_ref(&name_str->hdr, GcHeapKind::String);
    }
    if (message || message_len > 0) {
        if (!message && message_len > 0) {
            return nullptr;
        }
        message_str = gc_new_string(heap, message ? message : "", message_len);
        if (!message_str) {
            return nullptr;
        }
        *message_root = js_make_heap_ref(&message_str->hdr, GcHeapKind::String);
    }
    return gc_new_exception_unchecked(heap, position, name_str, message_str, *meta_root);
}

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, const char *name, std::size_t name_len,
                              const char *message, std::size_t message_len) {
    return gc_new_exception(heap, position, name, name_len, message, message_len, JsValue::make_undefined());
}

} // namespace fiber::script
