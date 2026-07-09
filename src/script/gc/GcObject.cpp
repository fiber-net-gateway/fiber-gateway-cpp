//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include "../../common/Assert.h"

#include <algorithm>
#include <new>
#include <utility>

namespace fiber::script {

using namespace gc_detail;

GcObject *gc_new_object(GcHeap *heap, std::size_t capacity) {
    FIBER_ASSERT(heap->no_gc_active());
    auto *hdr = gc_alloc_raw(heap, sizeof(GcObject), GcHeapKind::Object);
    if (!hdr) {
        return nullptr;
    }
    auto *obj = reinterpret_cast<GcObject *>(hdr);
    obj->size = 0;
    obj->version = 0;
    obj->entry_count = 0;
    obj->entry_capacity = capacity;
    obj->bucket_count = 0;
    obj->bucket_mask = 0;
    obj->head = -1;
    obj->tail = -1;
    obj->free_head = -1;
    obj->buckets = nullptr;
    obj->entries = nullptr;
    if (capacity > 0) {
        obj->entries = static_cast<GcObjectEntry *>(gc_alloc_extra(heap, entry_storage_bytes(capacity)));
        if (!obj->entries) {
            heap->alloc.free(obj);
            return nullptr;
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            obj->entries[i].key = nullptr;
            obj->entries[i].hash = 0;
            obj->entries[i].next_bucket = -1;
            obj->entries[i].prev_order = -1;
            obj->entries[i].next_order = -1;
            obj->entries[i].next_free = -1;
            obj->entries[i].occupied = false;
            std::construct_at(&obj->entries[i].value);
        }
        obj->bucket_count = bucket_count_for_entries(capacity);
        if (obj->bucket_count == 0) {
            for (std::size_t i = 0; i < capacity; ++i) {
                std::destroy_at(&obj->entries[i].value);
            }
            gc_free_extra(heap, obj->entries, entry_storage_bytes(capacity));
            heap->alloc.free(obj);
            return nullptr;
        }
        obj->buckets = static_cast<std::int32_t *>(gc_alloc_extra(heap, bucket_storage_bytes(obj->bucket_count)));
        if (!obj->buckets) {
            for (std::size_t i = 0; i < capacity; ++i) {
                std::destroy_at(&obj->entries[i].value);
            }
            gc_free_extra(heap, obj->entries, entry_storage_bytes(capacity));
            heap->alloc.free(obj);
            return nullptr;
        }
        for (std::size_t i = 0; i < obj->bucket_count; ++i) {
            obj->buckets[i] = -1;
        }
        obj->bucket_mask = obj->bucket_count - 1;
    }
    gc_link(heap, hdr);
    return obj;
}

bool gc_object_reserve(GcHeap *heap, GcObject *obj, std::size_t expected) {
    if (!obj) {
        return false;
    }
    FIBER_ASSERT(heap->no_gc_active());
    std::size_t new_capacity = obj->entry_capacity ? obj->entry_capacity : 1;
    if (expected > new_capacity) {
        while (new_capacity < expected) {
            new_capacity *= 2;
        }
        if (!grow_entries(heap, obj, new_capacity)) {
            return false;
        }
    } else if (obj->entry_capacity == 0 && expected > 0) {
        if (!grow_entries(heap, obj, expected)) {
            return false;
        }
    }
    std::size_t desired_bucket_count = bucket_count_for_entries(std::max(expected, obj->size));
    if (desired_bucket_count > obj->bucket_count) {
        if (!rehash_buckets(heap, obj, desired_bucket_count)) {
            return false;
        }
    } else if (obj->bucket_count == 0 && desired_bucket_count > 0) {
        if (!rehash_buckets(heap, obj, desired_bucket_count)) {
            return false;
        }
    }
    return true;
}

bool gc_object_set(GcHeap *heap, GcObject *obj, GcString *key, JsValue value) {
    if (!obj || !key) {
        return false;
    }
    FIBER_ASSERT(heap->no_gc_active());
    std::uint64_t hash = string_hash(key);
    int32_t existing = find_entry_index(obj, key, hash);
    if (existing != -1) {
        obj->entries[existing].value = std::move(value);
        return true;
    }
    if (!gc_object_reserve(heap, obj, obj->size + 1)) {
        return false;
    }
    if (obj->bucket_count == 0 || !obj->buckets || !obj->entries) {
        return false;
    }
    int32_t idx = allocate_entry(obj);
    if (idx == -1) {
        return false;
    }
    GcObjectEntry &entry = obj->entries[idx];
    entry.key = key;
    entry.value = std::move(value);
    entry.hash = hash;
    entry.occupied = true;
    entry.next_free = -1;
    std::size_t bucket = static_cast<std::size_t>(hash) & obj->bucket_mask;
    entry.next_bucket = obj->buckets[bucket];
    obj->buckets[bucket] = idx;
    entry.prev_order = obj->tail;
    entry.next_order = -1;
    if (obj->tail != -1) {
        obj->entries[obj->tail].next_order = idx;
    } else {
        obj->head = idx;
    }
    obj->tail = idx;
    obj->size += 1;
    obj->version += 1;
    return true;
}

const JsValue *gc_object_get(const GcObject *obj, const GcString *key) {
    if (!obj || !key) {
        return nullptr;
    }
    std::uint64_t hash = string_hash(key);
    int32_t idx = find_entry_index(obj, key, hash);
    if (idx == -1) {
        return nullptr;
    }
    return &obj->entries[idx].value;
}

bool gc_object_remove(GcObject *obj, const GcString *key) {
    if (!obj || !key || obj->bucket_count == 0 || !obj->buckets) {
        return false;
    }
    std::uint64_t hash = string_hash(key);
    std::size_t bucket = static_cast<std::size_t>(hash) & obj->bucket_mask;
    int32_t prev = -1;
    int32_t idx = obj->buckets[bucket];
    while (idx != -1) {
        GcObjectEntry &entry = obj->entries[idx];
        if (entry.occupied && entry.hash == hash && string_equals(entry.key, key)) {
            if (prev == -1) {
                obj->buckets[bucket] = entry.next_bucket;
            } else {
                obj->entries[prev].next_bucket = entry.next_bucket;
            }
            if (entry.prev_order != -1) {
                obj->entries[entry.prev_order].next_order = entry.next_order;
            } else {
                obj->head = entry.next_order;
            }
            if (entry.next_order != -1) {
                obj->entries[entry.next_order].prev_order = entry.prev_order;
            } else {
                obj->tail = entry.prev_order;
            }
            entry.occupied = false;
            entry.key = nullptr;
            entry.hash = 0;
            entry.next_bucket = -1;
            entry.prev_order = -1;
            entry.next_order = -1;
            entry.value = JsValue();
            entry.next_free = obj->free_head;
            obj->free_head = idx;
            obj->size -= 1;
            obj->version += 1;
            return true;
        }
        prev = idx;
        idx = entry.next_bucket;
    }
    return false;
}

// Insertion-order cursor over an object's entries. The order list (head / next_order)
// only contains live entries, so first/next never surface freed slots; callers may still
// guard on occupied/key defensively. Each step is O(1) - a full scan is O(n), unlike the
// old by-index gc_object_entry_at which walked from head on every lookup (O(n^2) per scan).
const GcObjectEntry *gc_object_first_entry(const GcObject *obj) {
    if (!obj || obj->head < 0) {
        return nullptr;
    }
    return &obj->entries[obj->head];
}

const GcObjectEntry *gc_object_next_entry(const GcObject *obj, const GcObjectEntry *entry) {
    if (!obj || !entry || entry->next_order < 0) {
        return nullptr;
    }
    return &obj->entries[entry->next_order];
}

} // namespace fiber::script
