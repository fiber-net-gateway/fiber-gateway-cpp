//
// Created by dear on 2025/12/30.
//

#include <fiber/script/gc/GcInternal.h>

#include <fiber/common/Assert.h>

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
constexpr std::size_t kStringInternInitialBucketCount = 64;
constexpr std::size_t kStringInternMaxLoad = 2;
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

[[nodiscard]] bool intern_eligible(std::size_t len) noexcept { return len <= kMaxInternStringLen; }

[[nodiscard]] std::size_t intern_bucket_bytes(std::size_t bucket_count) noexcept {
    if (bucket_count > std::numeric_limits<std::size_t>::max() / sizeof(GcString *)) {
        return 0;
    }
    return bucket_count * sizeof(GcString *);
}

[[nodiscard]] bool intern_ensure_buckets(GcHeap *heap, std::size_t desired_size) noexcept {
    if (!heap) {
        return false;
    }
    if (heap->string_intern_bucket_count != 0 &&
        desired_size <= heap->string_intern_bucket_count * kStringInternMaxLoad) {
        return true;
    }

    std::size_t new_count =
            heap->string_intern_bucket_count ? heap->string_intern_bucket_count : kStringInternInitialBucketCount;
    while (desired_size > new_count * kStringInternMaxLoad) {
        if (new_count > std::numeric_limits<std::size_t>::max() / 2) {
            return false;
        }
        new_count *= 2;
    }

    std::size_t bytes = intern_bucket_bytes(new_count);
    if (bytes == 0) {
        return false;
    }
    auto **new_buckets = static_cast<GcString **>(heap->alloc.alloc(bytes));
    if (!new_buckets) {
        return false;
    }
    for (std::size_t i = 0; i < new_count; ++i) {
        new_buckets[i] = nullptr;
    }

    if (heap->string_intern_buckets) {
        for (std::size_t i = 0; i < heap->string_intern_bucket_count; ++i) {
            GcString *cursor = heap->string_intern_buckets[i];
            while (cursor) {
                GcString *next = cursor->intern_next;
                std::size_t bucket = static_cast<std::size_t>(cursor->hash) & (new_count - 1);
                cursor->intern_next = new_buckets[bucket];
                new_buckets[bucket] = cursor;
                cursor = next;
            }
        }
        heap->alloc.free(heap->string_intern_buckets);
    }

    heap->string_intern_buckets = new_buckets;
    heap->string_intern_bucket_count = new_count;
    return true;
}

void intern_protect_hit(GcString *str) noexcept {
    if (str) {
        str->hdr.first_collect_protected = true;
    }
}

bool unlink_heap_obj(GcHeap *heap, GcHeader *target) noexcept {
    if (!heap || !target) {
        return false;
    }
    GcHeader **cursor = &heap->head;
    while (*cursor) {
        if (*cursor == target) {
            *cursor = target->next;
            target->next = nullptr;
            return true;
        }
        cursor = &(*cursor)->next;
    }
    return false;
}

std::uint64_t string_hash_wtf8(const char *data, std::size_t len) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    if (len == 0) {
        return hash;
    }
    if (!data) {
        return 0;
    }
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t string_hash(const GcString *str) {
    if (!str) {
        return 0;
    }
    if ((str->flags & kGcStringHashValid) != 0) {
        return str->hash;
    }
    auto *mutable_str = const_cast<GcString *>(str);
    mutable_str->hash = string_hash_wtf8(gc_string_wtf8_data(str), gc_string_byte_len(str));
    mutable_str->flags |= kGcStringHashValid;
    return mutable_str->hash;
}

bool string_equals(const GcString *lhs, const GcString *rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (!lhs || !rhs) {
        return false;
    }
    if (lhs->utf16_len != rhs->utf16_len) {
        return false;
    }
    const std::size_t lhs_len = gc_string_byte_len(lhs);
    const std::size_t rhs_len = gc_string_byte_len(rhs);
    if (lhs_len != rhs_len) {
        return false;
    }
    if (lhs_len == 0) {
        return true;
    }
    return std::memcmp(gc_string_wtf8_data(lhs), gc_string_wtf8_data(rhs), lhs_len) == 0;
}

GcString *gc_string_intern_lookup_wtf8(GcHeap *heap, const char *data, std::size_t byte_len, std::size_t utf16_len,
                                       std::uint64_t hash) noexcept {
    if (!heap || !intern_eligible(utf16_len) || (byte_len > 0 && !data) || !heap->string_intern_buckets ||
        heap->string_intern_bucket_count == 0) {
        return nullptr;
    }
    std::size_t bucket = static_cast<std::size_t>(hash) & (heap->string_intern_bucket_count - 1);
    for (GcString *cursor = heap->string_intern_buckets[bucket]; cursor; cursor = cursor->intern_next) {
        if (cursor->hash == hash && cursor->utf16_len == utf16_len && gc_string_byte_len(cursor) == byte_len &&
            (byte_len == 0 || std::memcmp(gc_string_wtf8_data(cursor), data, byte_len) == 0)) {
            intern_protect_hit(cursor);
            return cursor;
        }
    }
    return nullptr;
}

void gc_string_intern_insert(GcHeap *heap, GcString *str, std::uint64_t hash) noexcept {
    if (!heap || !str || !intern_eligible(str->utf16_len)) {
        return;
    }
    str->hash = hash;
    str->flags |= kGcStringHashValid;
    if (!intern_ensure_buckets(heap, heap->string_intern_size + 1) && !heap->string_intern_buckets) {
        return;
    }
    std::size_t bucket = static_cast<std::size_t>(hash) & (heap->string_intern_bucket_count - 1);
    str->intern_next = heap->string_intern_buckets[bucket];
    heap->string_intern_buckets[bucket] = str;
    heap->string_intern_size += 1;
}

GcString *gc_string_intern_final(GcHeap *heap, GcString *str) noexcept {
    if (!heap || !str || !intern_eligible(str->utf16_len)) {
        return str;
    }

    std::uint64_t hash = string_hash(str);
    GcString *existing =
            gc_string_intern_lookup_wtf8(heap, gc_string_wtf8_data(str), gc_string_byte_len(str), str->utf16_len, hash);
    if (existing && existing != str) {
        if (unlink_heap_obj(heap, &str->hdr)) {
            gc_free_obj(heap, &str->hdr);
        }
        return existing;
    }

    gc_string_intern_insert(heap, str, hash);
    return str;
}

void gc_string_intern_free_table(GcHeap *heap) noexcept {
    if (!heap || !heap->string_intern_buckets) {
        return;
    }
    heap->alloc.free(heap->string_intern_buckets);
    heap->string_intern_buckets = nullptr;
    heap->string_intern_bucket_count = 0;
    heap->string_intern_size = 0;
}

void gc_string_intern_remove(GcHeap *heap, GcString *str) noexcept {
    if (!heap || !str || !heap->string_intern_buckets || heap->string_intern_bucket_count == 0 ||
        !intern_eligible(str->utf16_len) || (str->flags & kGcStringHashValid) == 0) {
        return;
    }

    std::size_t bucket = static_cast<std::size_t>(str->hash) & (heap->string_intern_bucket_count - 1);
    GcString **cursor = &heap->string_intern_buckets[bucket];
    while (*cursor) {
        if (*cursor == str) {
            *cursor = str->intern_next;
            str->intern_next = nullptr;
            if (heap->string_intern_size > 0) {
                heap->string_intern_size -= 1;
            }
            if (heap->string_intern_size == 0) {
                gc_string_intern_free_table(heap);
            }
            return;
        }
        cursor = &(*cursor)->intern_next;
    }
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

std::size_t array_storage_bytes(std::size_t capacity) { return sizeof(JsValue) * capacity; }

std::size_t entry_storage_bytes(std::size_t capacity) { return sizeof(GcObjectEntry) * capacity; }

std::size_t bucket_storage_bytes(std::size_t bucket_count) { return sizeof(std::int32_t) * bucket_count; }

void gc_account_add(GcHeap *heap, std::size_t bytes) {
    if (bytes == 0) {
        return;
    }
    heap->bytes = saturating_add(heap->bytes, bytes);
}

void gc_account_sub(GcHeap *heap, std::size_t bytes) {
    if (bytes == 0) {
        return;
    }
    heap->bytes = (bytes >= heap->bytes) ? 0 : (heap->bytes - bytes);
}

void *gc_alloc_extra(GcHeap *heap, std::size_t bytes) {
    if (bytes == 0) {
        return nullptr;
    }
    FIBER_ASSERT(heap->no_gc_active());
    heap->maybe_collect_for_alloc(bytes);
    void *mem = heap->alloc.alloc(bytes);
    if (!mem) {
        return nullptr;
    }
    gc_account_add(heap, bytes);
    return mem;
}

void gc_free_extra(GcHeap *heap, void *ptr, std::size_t bytes) {
    if (!ptr) {
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
    if (!obj) {
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
    if (!obj || new_capacity == 0) {
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
    return str ? js_make_heap_ref(&str->hdr, GcHeapKind::String) : JsValue::make_undefined();
}

GcHeader *gc_alloc_raw(GcHeap *heap, std::size_t size, GcHeapKind kind) {
    FIBER_ASSERT(heap->no_gc_active());
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
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
        case GcHeapKind::String:
            break;
        case GcHeapKind::Binary:
            break;
        case GcHeapKind::Array: {
            auto *arr = reinterpret_cast<GcArray *>(obj);
            for (std::size_t i = 0; i < arr->size; ++i) {
                gc_mark_value(heap, arr->elems[i]);
            }
            break;
        }
        case GcHeapKind::Object: {
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
        case GcHeapKind::Exception: {
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
        case GcHeapKind::Iterator: {
            auto *iter = reinterpret_cast<GcIterator *>(obj);
            if (iter->kind == GcIteratorKind::Array) {
                if (iter->array) {
                    gc_mark_obj(heap, &iter->array->hdr);
                }
            } else if (iter->object) {
                gc_mark_obj(heap, &iter->object->hdr);
            }
            if (iter->has_current) {
                gc_mark_value(heap, iter->current_key);
                gc_mark_value(heap, iter->current_value);
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
        case GcHeapKind::String: {
            auto *str = reinterpret_cast<GcString *>(obj);
            gc_string_intern_remove(heap, str);
            break;
        }
        case GcHeapKind::Binary: {
            auto *bin = reinterpret_cast<GcBinary *>(obj);
            if (bin->data) {
                gc_free_extra(heap, bin->data, bin->len);
            }
            break;
        }
        case GcHeapKind::Array: {
            auto *arr = reinterpret_cast<GcArray *>(obj);
            if (arr->elems) {
                for (std::size_t i = 0; i < arr->capacity; ++i) {
                    std::destroy_at(&arr->elems[i]);
                }
                gc_free_extra(heap, arr->elems, array_storage_bytes(arr->capacity));
            }
            break;
        }
        case GcHeapKind::Object: {
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
        case GcHeapKind::Exception: {
            auto *exc = reinterpret_cast<GcException *>(obj);
            std::destroy_at(&exc->meta);
            break;
        }
        case GcHeapKind::Iterator: {
            // GcIterator holds only borrowed pointers into other GC objects and
            // trivially-copyable JsValues; there is no extra storage to free.
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
