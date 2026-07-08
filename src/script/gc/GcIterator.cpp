//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include "../../common/Assert.h"

#include <new>
#include <utility>

namespace fiber::script {

using namespace gc_detail;

GcIterator *gc_new_array_iterator(GcHeap *heap, GcArray *array, GcIteratorMode mode) {
    if (!heap) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcIterator), GcHeapKind::Iterator);
    if (!hdr) {
        return nullptr;
    }
    auto *iter = reinterpret_cast<GcIterator *>(hdr);
    iter->kind = GcIteratorKind::Array;
    iter->mode = mode;
    iter->expected_version = array ? array->version : 0;
    iter->using_snapshot = false;
    iter->array = array;
    iter->object = nullptr;
    iter->index = 0;
    iter->cursor = -1;
    iter->snapshot_keys = nullptr;
    iter->snapshot_size = 0;
    iter->snapshot_index = 0;
    std::construct_at(&iter->current_key, JsValue::make_undefined());
    std::construct_at(&iter->current_value, JsValue::make_undefined());
    iter->has_current = false;
    gc_link(heap, hdr);
    return iter;
}

GcIterator *gc_new_object_iterator(GcHeap *heap, GcObject *object, GcIteratorMode mode) {
    if (!heap) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcIterator), GcHeapKind::Iterator);
    if (!hdr) {
        return nullptr;
    }
    auto *iter = reinterpret_cast<GcIterator *>(hdr);
    iter->kind = GcIteratorKind::Object;
    iter->mode = mode;
    iter->expected_version = object ? object->version : 0;
    iter->using_snapshot = false;
    iter->array = nullptr;
    iter->object = object;
    iter->index = 0;
    iter->cursor = object ? object->head : -1;
    iter->snapshot_keys = nullptr;
    iter->snapshot_size = 0;
    iter->snapshot_index = 0;
    std::construct_at(&iter->current_key, JsValue::make_undefined());
    std::construct_at(&iter->current_value, JsValue::make_undefined());
    iter->has_current = false;
    gc_link(heap, hdr);
    return iter;
}

bool gc_iterator_next(GcHeap *heap, GcIterator *iter, JsValue &out, bool &done) {
    out = JsValue::make_undefined();
    done = true;
    if (!heap || !iter) {
        return false;
    }
    FIBER_ASSERT(heap->no_gc_active());
    iter->has_current = false;
    iter->current_key = JsValue::make_undefined();
    iter->current_value = JsValue::make_undefined();
    if (iter->kind == GcIteratorKind::Array) {
        if (!iter->array) {
            return true;
        }
        GcArray *arr = iter->array;
        if (arr->version != iter->expected_version) {
            iter->expected_version = arr->version;
        }
        if (iter->index >= arr->size) {
            return true;
        }
        std::size_t idx = iter->index;
        done = false;
        switch (iter->mode) {
            case GcIteratorMode::Keys:
                out = JsValue::make_integer(static_cast<int64_t>(idx));
                iter->current_key = out;
                iter->has_current = true;
                iter->index = idx + 1;
                return true;
            case GcIteratorMode::Values:
                out = arr->elems[idx];
                iter->current_value = out;
                iter->current_key = JsValue::make_integer(static_cast<int64_t>(idx));
                iter->has_current = true;
                iter->index = idx + 1;
                return true;
            case GcIteratorMode::Entries: {
                JsValue key = JsValue::make_integer(static_cast<int64_t>(idx));
                if (!build_entry_array(heap, key, arr->elems[idx], out)) {
                    return false;
                }
                iter->current_key = key;
                iter->current_value = arr->elems[idx];
                iter->has_current = true;
                iter->index = idx + 1;
                return true;
            }
        }
        return true;
    }
    if (!iter->object) {
        return true;
    }
    GcObject *obj = iter->object;
    if (!iter->using_snapshot && iter->expected_version != obj->version) {
        if (!build_object_snapshot(heap, iter, obj)) {
            return false;
        }
    }
    if (iter->using_snapshot) {
        if (iter->snapshot_index >= iter->snapshot_size) {
            return true;
        }
        std::size_t snapshot_index = iter->snapshot_index;
        GcString *key = iter->snapshot_keys[snapshot_index];
        if (!key) {
            iter->snapshot_index = snapshot_index + 1;
            return true;
        }
        JsValue key_value = make_heap_string_value(key);
        const JsValue *found = gc_object_get(obj, key);
        JsValue value = found ? *found : JsValue::make_undefined();
        done = false;
        switch (iter->mode) {
            case GcIteratorMode::Keys:
                out = key_value;
                iter->current_key = out;
                iter->has_current = true;
                iter->snapshot_index = snapshot_index + 1;
                return true;
            case GcIteratorMode::Values:
                out = value;
                iter->current_value = out;
                iter->current_key = key_value;
                iter->has_current = true;
                iter->snapshot_index = snapshot_index + 1;
                return true;
            case GcIteratorMode::Entries:
                if (!build_entry_array(heap, key_value, value, out)) {
                    return false;
                }
                iter->current_key = key_value;
                iter->current_value = value;
                iter->has_current = true;
                iter->snapshot_index = snapshot_index + 1;
                return true;
        }
        return true;
    }
    while (iter->cursor != -1) {
        int32_t cursor = iter->cursor;
        GcObjectEntry &entry = obj->entries[cursor];
        int32_t next_cursor = entry.next_order;
        if (!entry.occupied || !entry.key) {
            iter->cursor = next_cursor;
            continue;
        }
        JsValue key_value = make_heap_string_value(entry.key);
        done = false;
        switch (iter->mode) {
            case GcIteratorMode::Keys:
                out = key_value;
                iter->current_key = out;
                iter->has_current = true;
                iter->cursor = next_cursor;
                return true;
            case GcIteratorMode::Values:
                out = entry.value;
                iter->current_value = out;
                iter->current_key = key_value;
                iter->has_current = true;
                iter->cursor = next_cursor;
                return true;
            case GcIteratorMode::Entries:
                if (!build_entry_array(heap, key_value, entry.value, out)) {
                    return false;
                }
                iter->current_key = key_value;
                iter->current_value = entry.value;
                iter->has_current = true;
                iter->cursor = next_cursor;
                return true;
        }
        return true;
    }
    return true;
}

} // namespace fiber::script
