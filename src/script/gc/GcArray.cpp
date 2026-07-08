//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include "../../common/Assert.h"

#include <cstring>
#include <new>
#include <utility>

namespace fiber::script {

using namespace gc_detail;

GcBinary *gc_new_binary(GcHeap *heap, const std::uint8_t *data, std::size_t len) {
    if (!heap) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcBinary), GcHeapKind::Binary);
    if (!hdr) {
        return nullptr;
    }
    auto *bin = reinterpret_cast<GcBinary *>(hdr);
    bin->len = len;
    bin->data = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(bin);
        return nullptr;
    }
    if (len > 0) {
        bin->data = static_cast<std::uint8_t *>(gc_alloc_extra(heap, len));
        if (!bin->data) {
            heap->alloc.free(bin);
            return nullptr;
        }
        std::memcpy(bin->data, data, len);
    }
    gc_link(heap, hdr);
    return bin;
}

GcArray *gc_new_array(GcHeap *heap, std::size_t capacity) {
    if (!heap) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcArray), GcHeapKind::Array);
    if (!hdr) {
        return nullptr;
    }
    auto *arr = reinterpret_cast<GcArray *>(hdr);
    arr->size = 0;
    arr->capacity = capacity;
    arr->version = 0;
    arr->elems = nullptr;
    if (capacity > 0) {
        arr->elems = static_cast<JsValue *>(gc_alloc_extra(heap, array_storage_bytes(capacity)));
        if (!arr->elems) {
            heap->alloc.free(arr);
            return nullptr;
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            std::construct_at(&arr->elems[i]);
        }
    }
    gc_link(heap, hdr);
    return arr;
}

bool gc_array_reserve(GcHeap *heap, GcArray *arr, std::size_t expected) {
    if (!heap || !arr) {
        return false;
    }
    FIBER_ASSERT(heap->no_gc_active());
    if (expected <= arr->capacity) {
        return true;
    }
    std::size_t new_capacity = arr->capacity ? arr->capacity * 2 : 1;
    while (new_capacity < expected) {
        new_capacity *= 2;
    }
    auto *new_elems = static_cast<JsValue *>(gc_alloc_extra(heap, array_storage_bytes(new_capacity)));
    if (!new_elems) {
        return false;
    }
    for (std::size_t i = 0; i < new_capacity; ++i) {
        std::construct_at(&new_elems[i]);
    }
    for (std::size_t i = 0; i < arr->size; ++i) {
        new_elems[i] = std::move(arr->elems[i]);
    }
    if (arr->elems) {
        for (std::size_t i = 0; i < arr->capacity; ++i) {
            std::destroy_at(&arr->elems[i]);
        }
        gc_free_extra(heap, arr->elems, array_storage_bytes(arr->capacity));
    }
    arr->elems = new_elems;
    arr->capacity = new_capacity;
    return true;
}

const JsValue *gc_array_get(const GcArray *arr, std::size_t index) {
    if (!arr || index >= arr->size) {
        return nullptr;
    }
    return &arr->elems[index];
}

bool gc_array_set(GcHeap *heap, GcArray *arr, std::size_t index, JsValue value) {
    if (!heap || !arr) {
        return false;
    }
    FIBER_ASSERT(heap->no_gc_active());
    if (index < arr->size) {
        arr->elems[index] = std::move(value);
        return true;
    }
    if (!gc_array_reserve(heap, arr, index + 1)) {
        return false;
    }
    while (arr->size < index) {
        arr->elems[arr->size] = JsValue::make_undefined();
        arr->size += 1;
    }
    arr->elems[arr->size] = std::move(value);
    arr->size += 1;
    arr->version += 1;
    return true;
}

bool gc_array_push(GcHeap *heap, GcArray *arr, JsValue value) {
    if (!heap || !arr) {
        return false;
    }
    FIBER_ASSERT(heap->no_gc_active());
    if (!gc_array_reserve(heap, arr, arr->size + 1)) {
        return false;
    }
    arr->elems[arr->size] = std::move(value);
    arr->size += 1;
    arr->version += 1;
    return true;
}

bool gc_array_pop(GcArray *arr, JsValue *out) {
    if (!arr || arr->size == 0) {
        return false;
    }
    std::size_t idx = arr->size - 1;
    JsValue removed = std::move(arr->elems[idx]);
    arr->elems[idx] = JsValue::make_undefined();
    arr->size -= 1;
    arr->version += 1;
    if (out) {
        *out = std::move(removed);
    }
    return true;
}

bool gc_array_insert(GcHeap *heap, GcArray *arr, std::size_t index, JsValue value) {
    if (!heap || !arr) {
        return false;
    }
    FIBER_ASSERT(heap->no_gc_active());
    if (index > arr->size) {
        index = arr->size;
    }
    if (!gc_array_reserve(heap, arr, arr->size + 1)) {
        return false;
    }
    for (std::size_t i = arr->size; i > index; --i) {
        arr->elems[i] = std::move(arr->elems[i - 1]);
    }
    arr->elems[index] = std::move(value);
    arr->size += 1;
    arr->version += 1;
    return true;
}

bool gc_array_remove(GcArray *arr, std::size_t index, JsValue *out) {
    if (!arr || index >= arr->size) {
        return false;
    }
    if (out) {
        *out = std::move(arr->elems[index]);
    }
    for (std::size_t i = index + 1; i < arr->size; ++i) {
        arr->elems[i - 1] = std::move(arr->elems[i]);
    }
    arr->elems[arr->size - 1] = JsValue::make_undefined();
    arr->size -= 1;
    arr->version += 1;
    return true;
}

} // namespace fiber::script
