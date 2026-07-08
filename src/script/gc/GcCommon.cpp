//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace fiber::script::gc_detail {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kMinBucketCount = 8;
constexpr std::size_t kMaxLoadNumerator = 3;
constexpr std::size_t kMaxLoadDenominator = 4;
constexpr std::size_t kMinGcThreshold = 1 << 20;

std::size_t saturating_add(std::size_t lhs, std::size_t rhs) {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs + rhs;
}

std::size_t next_threshold(std::size_t live_bytes) {
    std::size_t grown = live_bytes;
    if (grown <= (std::numeric_limits<std::size_t>::max() >> 1)) {
        grown *= 2;
    } else {
        grown = std::numeric_limits<std::size_t>::max();
    }
    return grown < kMinGcThreshold ? kMinGcThreshold : grown;
}

GcMark flip_mark(GcMark mark) { return (mark == GcMark::GcMark_0) ? GcMark::GcMark_1 : GcMark::GcMark_0; }

std::uint64_t hash_code_units(const GcString *str) {
    std::uint64_t hash = kFnvOffsetBasis;
    if (!str || str->len == 0) {
        return hash;
    }
    if (str->encoding == GcStringEncoding::Byte) {
        for (std::size_t i = 0; i < str->len; ++i) {
            hash ^= static_cast<std::uint16_t>(str->data8[i]);
            hash *= kFnvPrime;
        }
        return hash;
    }
    for (std::size_t i = 0; i < str->len; ++i) {
        hash ^= static_cast<std::uint16_t>(str->data16[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t string_hash(const GcString *str) {
    if (!str) {
        return 0;
    }
    if (str->hash_valid) {
        return str->hash;
    }
    auto *mutable_str = const_cast<GcString *>(str);
    mutable_str->hash = hash_code_units(str);
    mutable_str->hash_valid = true;
    return mutable_str->hash;
}

bool string_equals(const GcString *lhs, const GcString *rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (!lhs || !rhs) {
        return false;
    }
    if (lhs->len != rhs->len) {
        return false;
    }
    if (lhs->len == 0) {
        return true;
    }
    if (lhs->encoding == rhs->encoding) {
        if (lhs->encoding == GcStringEncoding::Byte) {
            return std::memcmp(lhs->data8, rhs->data8, lhs->len) == 0;
        }
        return std::memcmp(lhs->data16, rhs->data16, lhs->len * sizeof(char16_t)) == 0;
    }
    if (lhs->encoding == GcStringEncoding::Byte) {
        for (std::size_t i = 0; i < lhs->len; ++i) {
            if (rhs->data16[i] != static_cast<char16_t>(lhs->data8[i])) {
                return false;
            }
        }
        return true;
    }
    for (std::size_t i = 0; i < lhs->len; ++i) {
        if (lhs->data16[i] != static_cast<char16_t>(rhs->data8[i])) {
            return false;
        }
    }
    return true;
}

std::size_t next_pow2(std::size_t value) {
    if (value <= 1) {
        return 1;
    }
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

std::size_t bucket_count_for_entries(std::size_t entry_capacity) {
    if (entry_capacity == 0) {
        return 0;
    }
    std::size_t needed = (entry_capacity * kMaxLoadDenominator + kMaxLoadNumerator - 1) / kMaxLoadNumerator;
    if (needed < kMinBucketCount) {
        needed = kMinBucketCount;
    }
    return next_pow2(needed);
}

std::size_t string_storage_bytes(std::size_t len, GcStringEncoding encoding) {
    if (encoding == GcStringEncoding::Utf16) {
        if (len > (std::numeric_limits<std::size_t>::max() / sizeof(char16_t)) - 1) {
            return 0;
        }
        return sizeof(char16_t) * (len + 1);
    }
    if (len == std::numeric_limits<std::size_t>::max()) {
        return 0;
    }
    return len + 1;
}

std::size_t array_storage_bytes(std::size_t capacity) { return sizeof(JsValue) * capacity; }

std::size_t entry_storage_bytes(std::size_t capacity) { return sizeof(GcObjectEntry) * capacity; }

std::size_t bucket_storage_bytes(std::size_t bucket_count) { return sizeof(std::int32_t) * bucket_count; }

void gc_account_add(GcHeap *heap, std::size_t bytes) {
    if (!heap || bytes == 0) {
        return;
    }
    heap->bytes = saturating_add(heap->bytes, bytes);
}

void gc_account_sub(GcHeap *heap, std::size_t bytes) {
    if (!heap || bytes == 0) {
        return;
    }
    heap->bytes = (bytes >= heap->bytes) ? 0 : (heap->bytes - bytes);
}

void *gc_alloc_extra(GcHeap *heap, std::size_t bytes) {
    if (!heap || bytes == 0) {
        return nullptr;
    }
    heap->maybe_collect_for_alloc(bytes);
    void *mem = heap->alloc.alloc(bytes);
    if (!mem) {
        return nullptr;
    }
    gc_account_add(heap, bytes);
    return mem;
}

void gc_free_extra(GcHeap *heap, void *ptr, std::size_t bytes) {
    if (!heap || !ptr) {
        return;
    }
    heap->alloc.free(ptr);
    gc_account_sub(heap, bytes);
}

int32_t find_entry_index(const GcObject *obj, const GcString *key, std::uint64_t hash) {
    if (!obj || obj->bucket_count == 0 || !obj->buckets) {
        return -1;
    }
    std::size_t bucket = static_cast<std::size_t>(hash) & obj->bucket_mask;
    int32_t idx = obj->buckets[bucket];
    while (idx != -1) {
        const GcObjectEntry &entry = obj->entries[idx];
        if (entry.occupied && entry.hash == hash && string_equals(entry.key, key)) {
            return idx;
        }
        idx = entry.next_bucket;
    }
    return -1;
}

bool rehash_buckets(GcHeap *heap, GcObject *obj, std::size_t new_bucket_count) {
    if (!heap || !obj) {
        return false;
    }
    if (new_bucket_count == 0) {
        return false;
    }
    auto *new_buckets = static_cast<std::int32_t *>(gc_alloc_extra(heap, bucket_storage_bytes(new_bucket_count)));
    if (!new_buckets) {
        return false;
    }
    for (std::size_t i = 0; i < new_bucket_count; ++i) {
        new_buckets[i] = -1;
    }
    for (std::size_t i = 0; i < obj->entry_count; ++i) {
        GcObjectEntry &entry = obj->entries[i];
        if (!entry.occupied) {
            entry.next_bucket = -1;
            continue;
        }
        std::size_t bucket = static_cast<std::size_t>(entry.hash) & (new_bucket_count - 1);
        entry.next_bucket = new_buckets[bucket];
        new_buckets[bucket] = static_cast<std::int32_t>(i);
    }
    if (obj->buckets) {
        gc_free_extra(heap, obj->buckets, bucket_storage_bytes(obj->bucket_count));
    }
    obj->buckets = new_buckets;
    obj->bucket_count = new_bucket_count;
    obj->bucket_mask = new_bucket_count - 1;
    return true;
}

bool grow_entries(GcHeap *heap, GcObject *obj, std::size_t new_capacity) {
    if (!heap || !obj || new_capacity == 0) {
        return false;
    }
    auto *new_entries = static_cast<GcObjectEntry *>(gc_alloc_extra(heap, entry_storage_bytes(new_capacity)));
    if (!new_entries) {
        return false;
    }
    for (std::size_t i = 0; i < new_capacity; ++i) {
        new_entries[i].key = nullptr;
        new_entries[i].hash = 0;
        new_entries[i].next_bucket = -1;
        new_entries[i].prev_order = -1;
        new_entries[i].next_order = -1;
        new_entries[i].next_free = -1;
        new_entries[i].occupied = false;
        std::construct_at(&new_entries[i].value);
    }
    for (std::size_t i = 0; i < obj->entry_count; ++i) {
        new_entries[i].key = obj->entries[i].key;
        new_entries[i].hash = obj->entries[i].hash;
        new_entries[i].next_bucket = obj->entries[i].next_bucket;
        new_entries[i].prev_order = obj->entries[i].prev_order;
        new_entries[i].next_order = obj->entries[i].next_order;
        new_entries[i].next_free = obj->entries[i].next_free;
        new_entries[i].occupied = obj->entries[i].occupied;
        new_entries[i].value = std::move(obj->entries[i].value);
    }
    if (obj->entries) {
        for (std::size_t i = 0; i < obj->entry_capacity; ++i) {
            std::destroy_at(&obj->entries[i].value);
        }
        gc_free_extra(heap, obj->entries, entry_storage_bytes(obj->entry_capacity));
    }
    obj->entries = new_entries;
    obj->entry_capacity = new_capacity;
    return true;
}

int32_t allocate_entry(GcObject *obj) {
    if (!obj) {
        return -1;
    }
    if (obj->free_head != -1) {
        int32_t idx = obj->free_head;
        obj->free_head = obj->entries[idx].next_free;
        obj->entries[idx].next_free = -1;
        return idx;
    }
    if (obj->entry_count >= obj->entry_capacity) {
        return -1;
    }
    int32_t idx = static_cast<int32_t>(obj->entry_count);
    obj->entry_count += 1;
    return idx;
}

JsValue make_heap_string_value(GcString *str) {
    return str ? js_make_heap_ref(&str->hdr, JsHeapKind::String) : JsValue::make_undefined();
}

bool build_entry_array(GcHeap *heap, const JsValue &key, const JsValue &value, JsValue &out) {
    if (!heap) {
        return false;
    }
    JsValue result = JsValue::make_array(*heap, 2);
    if (js_value_type(result) != JsNodeType::Array) {
        return false;
    }
    auto *arr = js_value_heap_ptr<GcArray>(result);
    arr->elems[0] = key;
    arr->elems[1] = value;
    arr->size = 2;
    arr->version += 1;
    out = std::move(result);
    return true;
}

bool build_object_snapshot(GcHeap *heap, GcIterator *iter, const GcObject *obj) {
    if (!heap || !iter || !obj) {
        return false;
    }
    iter->snapshot_index = 0;
    iter->snapshot_size = 0;
    iter->snapshot_keys = nullptr;
    std::size_t count = 0;
    for (int32_t cursor = iter->cursor; cursor != -1; cursor = obj->entries[cursor].next_order) {
        const GcObjectEntry &entry = obj->entries[cursor];
        if (entry.occupied && entry.key) {
            count += 1;
        }
    }
    if (count == 0) {
        return true;
    }
    auto **keys = static_cast<GcString **>(gc_alloc_extra(heap, sizeof(GcString *) * count));
    if (!keys) {
        return false;
    }
    std::size_t idx = 0;
    for (int32_t cursor = iter->cursor; cursor != -1; cursor = obj->entries[cursor].next_order) {
        const GcObjectEntry &entry = obj->entries[cursor];
        if (entry.occupied && entry.key) {
            keys[idx++] = entry.key;
        }
    }
    iter->using_snapshot = true;
    iter->snapshot_keys = keys;
    iter->snapshot_size = idx;
    return true;
}

GcHeader *gc_alloc_raw(GcHeap *heap, std::size_t size, GcKind kind) {
    heap->maybe_collect_for_alloc(size);
    void *mem = heap->alloc.alloc(size);
    if (!mem) {
        return nullptr;
    }
    auto *hdr = static_cast<GcHeader *>(mem);
    hdr->next = nullptr;
    hdr->mark_ = heap->live_mark;
    hdr->first_collect_protected = false;
    hdr->kind = kind;
    hdr->size_ = static_cast<std::uint32_t>(size);
    return hdr;
}

void gc_link(GcHeap *heap, GcHeader *hdr) {
    // Start the first-collect guard only when the object enters the GC list. Pre-link
    // extra allocations can trigger collection, but they cannot spend this protection.
    hdr->mark_ = flip_mark(heap->live_mark);
    hdr->first_collect_protected = true;
    hdr->next = heap->head;
    heap->head = hdr;
    gc_account_add(heap, hdr->size_);
}

void gc_mark_obj(GcHeap *heap, GcHeader *obj);

void gc_mark_value(GcHeap *heap, const JsValue &value) {
    const GcHeader *hdr = js_value_heap_header(value);
    if (hdr) {
        gc_mark_obj(heap, const_cast<GcHeader *>(hdr));
    }
}

void gc_trace_children(GcHeap *heap, GcHeader *obj) {
    switch (obj->kind) {
        case GcKind::String:
            break;
        case GcKind::Binary:
            break;
        case GcKind::Array: {
            auto *arr = reinterpret_cast<GcArray *>(obj);
            for (std::size_t i = 0; i < arr->size; ++i) {
                gc_mark_value(heap, arr->elems[i]);
            }
            break;
        }
        case GcKind::Object: {
            auto *objv = reinterpret_cast<GcObject *>(obj);
            int32_t cursor = objv->head;
            while (cursor != -1) {
                const GcObjectEntry &entry = objv->entries[cursor];
                if (entry.key) {
                    gc_mark_obj(heap, &entry.key->hdr);
                }
                gc_mark_value(heap, entry.value);
                cursor = entry.next_order;
            }
            break;
        }
        case GcKind::Exception: {
            auto *exc = reinterpret_cast<GcException *>(obj);
            if (exc->name) {
                gc_mark_obj(heap, &exc->name->hdr);
            }
            if (exc->message) {
                gc_mark_obj(heap, &exc->message->hdr);
            }
            gc_mark_value(heap, exc->meta);
            break;
        }
        case GcKind::Iterator: {
            auto *iter = reinterpret_cast<GcIterator *>(obj);
            if (iter->array) {
                gc_mark_obj(heap, &iter->array->hdr);
            }
            if (iter->object) {
                gc_mark_obj(heap, &iter->object->hdr);
            }
            if (iter->has_current) {
                gc_mark_value(heap, iter->current_key);
                gc_mark_value(heap, iter->current_value);
            }
            for (std::size_t i = 0; i < iter->snapshot_size; ++i) {
                if (iter->snapshot_keys && iter->snapshot_keys[i]) {
                    gc_mark_obj(heap, &iter->snapshot_keys[i]->hdr);
                }
            }
            break;
        }
    }
}

void gc_mark_obj(GcHeap *heap, GcHeader *obj) {
    if (!obj) {
        return;
    }
    const bool force_trace = obj->first_collect_protected;
    if (obj->mark_ == heap->live_mark && !force_trace) {
        return;
    }
    obj->first_collect_protected = false;
    obj->mark_ = heap->live_mark;
    gc_trace_children(heap, obj);
}

void gc_free_obj(GcHeap *heap, GcHeader *obj) {
    switch (obj->kind) {
        case GcKind::String: {
            auto *str = reinterpret_cast<GcString *>(obj);
            if (str->encoding == GcStringEncoding::Utf16) {
                if (str->data16) {
                    gc_free_extra(heap, str->data16, string_storage_bytes(str->len, GcStringEncoding::Utf16));
                }
            } else if (str->data8) {
                gc_free_extra(heap, str->data8, string_storage_bytes(str->len, GcStringEncoding::Byte));
            }
            break;
        }
        case GcKind::Binary: {
            auto *bin = reinterpret_cast<GcBinary *>(obj);
            if (bin->data) {
                gc_free_extra(heap, bin->data, bin->len);
            }
            break;
        }
        case GcKind::Array: {
            auto *arr = reinterpret_cast<GcArray *>(obj);
            if (arr->elems) {
                for (std::size_t i = 0; i < arr->capacity; ++i) {
                    std::destroy_at(&arr->elems[i]);
                }
                gc_free_extra(heap, arr->elems, array_storage_bytes(arr->capacity));
            }
            break;
        }
        case GcKind::Object: {
            auto *objv = reinterpret_cast<GcObject *>(obj);
            if (objv->entries) {
                for (std::size_t i = 0; i < objv->entry_capacity; ++i) {
                    std::destroy_at(&objv->entries[i].value);
                }
                gc_free_extra(heap, objv->entries, entry_storage_bytes(objv->entry_capacity));
            }
            if (objv->buckets) {
                gc_free_extra(heap, objv->buckets, bucket_storage_bytes(objv->bucket_count));
            }
            break;
        }
        case GcKind::Exception: {
            auto *exc = reinterpret_cast<GcException *>(obj);
            std::destroy_at(&exc->meta);
            break;
        }
        case GcKind::Iterator: {
            auto *iter = reinterpret_cast<GcIterator *>(obj);
            if (iter->snapshot_keys) {
                gc_free_extra(heap, iter->snapshot_keys, sizeof(GcString *) * iter->snapshot_size);
            }
            std::destroy_at(&iter->current_key);
            std::destroy_at(&iter->current_value);
            break;
        }
    }
    gc_account_sub(heap, obj->size_);
    heap->alloc.free(obj);
}

void gc_sweep_unmarked(GcHeap *heap) {
    GcHeader **cursor = &heap->head;
    while (*cursor) {
        GcHeader *obj = *cursor;
        if (obj->mark_ != heap->live_mark) {
            *cursor = obj->next;
            gc_free_obj(heap, obj);
        } else {
            cursor = &obj->next;
        }
    }
}

} // namespace fiber::script::gc_detail
