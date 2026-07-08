//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include "../../common/Assert.h"

namespace fiber::script {

using namespace gc_detail;

GcIterator *gc_new_array_iterator(GcHeap *heap, GcArray *array) {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcIterator), GcHeapKind::Iterator);
    if (!hdr) {
        return nullptr;
    }
    auto *iter = reinterpret_cast<GcIterator *>(hdr);
    iter->kind = GcIteratorKind::Array;
    iter->has_current = false;
    iter->expected_version = array ? array->version : 0;
    iter->current_key = JsValue::make_undefined();
    iter->current_value = JsValue::make_undefined();
    iter->array = array;
    iter->index = 0;
    gc_link(heap, hdr);
    return iter;
}

GcIterator *gc_new_object_iterator(GcHeap *heap, GcObject *object) {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcIterator), GcHeapKind::Iterator);
    if (!hdr) {
        return nullptr;
    }
    auto *iter = reinterpret_cast<GcIterator *>(hdr);
    iter->kind = GcIteratorKind::Object;
    iter->has_current = false;
    iter->expected_version = object ? object->version : 0;
    iter->current_key = JsValue::make_undefined();
    iter->current_value = JsValue::make_undefined();
    iter->object = object;
    iter->cursor = object ? object->head : -1;
    gc_link(heap, hdr);
    return iter;
}

GcIterStep gc_iterator_next(GcHeap *heap, GcIterator *iter) {
    if (!iter) {
        return GcIterStep::Done;
    }
    FIBER_ASSERT(heap->no_gc_active());
    iter->has_current = false;
    iter->current_key = JsValue::make_undefined();
    iter->current_value = JsValue::make_undefined();
    if (iter->kind == GcIteratorKind::Array) {
        GcArray *arr = iter->array;
        if (!arr) {
            return GcIterStep::Done;
        }
        if (arr->version != iter->expected_version) {
            return GcIterStep::Mutated;
        }
        if (iter->index >= arr->size) {
            return GcIterStep::Done;
        }
        std::size_t idx = iter->index;
        iter->current_value = arr->elems[idx];
        iter->current_key = JsValue::make_integer(static_cast<std::int64_t>(idx));
        iter->has_current = true;
        iter->index = idx + 1;
        return GcIterStep::Item;
    }
    GcObject *obj = iter->object;
    if (!obj) {
        return GcIterStep::Done;
    }
    if (obj->version != iter->expected_version) {
        return GcIterStep::Mutated;
    }
    while (iter->cursor != -1) {
        std::int32_t cursor = iter->cursor;
        const GcObjectEntry &entry = obj->entries[cursor];
        std::int32_t next_cursor = entry.next_order;
        if (!entry.occupied || !entry.key) {
            iter->cursor = next_cursor;
            continue;
        }
        iter->current_key = make_heap_string_value(entry.key);
        iter->current_value = entry.value;
        iter->has_current = true;
        iter->cursor = next_cursor;
        return GcIterStep::Item;
    }
    return GcIterStep::Done;
}

} // namespace fiber::script
