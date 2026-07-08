//
// Created by dear on 2025/12/30.
//

#include "JsGc.h"

#include "../common/Assert.h"
#include "../common/json/Utf.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace fiber::script {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kMinBucketCount = 8;
constexpr std::size_t kMaxLoadNumerator = 3;
constexpr std::size_t kMaxLoadDenominator = 4;
constexpr std::size_t kMinGcThreshold = 1 << 20;
constexpr std::size_t kValueBlockSlots = 8;

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
    if (heap->threshold && saturating_add(heap->bytes, bytes) >= heap->threshold) {
        heap->collect();
    }
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
    if (heap->threshold && saturating_add(heap->bytes, size) >= heap->threshold) {
        heap->collect();
    }
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

} // namespace

GcHeap::GcHeap() : pool_(&owned_pool_) {}

GcHeap::GcHeap(fiber::mem::BufPool &pool) : pool_(&pool) {}

GcHeap::~GcHeap() {
    while (head) {
        GcHeader *obj = head;
        head = obj->next;
        gc_free_obj(this, obj);
    }
}

ValueHandle GcHeap::local_value() {
    if (!local_current_ || local_top_ == local_end_) {
        ValueBlock *block = acquire_local_block();
        if (!block) {
            return nullptr;
        }
        if (!local_head_) {
            local_head_ = block;
        } else if (local_current_) {
            local_current_->next = block;
        }
        local_current_ = block;
        local_top_ = block->slots;
        local_end_ = block->slots + kValueBlockSlots;
    }
    ValueHandle handle = local_top_++;
    *handle = fiber::script::JsValue::make_undefined();
    return handle;
}

ValueHandle GcHeap::global_value() {
    if (!global_current_ || global_top_ == global_end_) {
        ValueBlock *block = acquire_global_block();
        if (!block) {
            return nullptr;
        }
        if (!global_head_) {
            global_head_ = block;
        } else if (global_current_) {
            global_current_->next = block;
        }
        global_current_ = block;
        global_top_ = block->slots;
        global_end_ = block->slots + kValueBlockSlots;
    }
    ValueHandle handle = global_top_++;
    *handle = fiber::script::JsValue::make_undefined();
    return handle;
}

void GcHeap::visit_roots(fiber::script::GcRootVisitor &visitor) noexcept {
    for (ValueBlock *block = local_head_; block; block = block->next) {
        const std::size_t count =
                block == local_current_ ? static_cast<std::size_t>(local_top_ - block->slots) : kValueBlockSlots;
        visitor.visit_range(block->slots, count);
        if (block == local_current_) {
            break;
        }
    }
    for (ValueBlock *block = global_head_; block; block = block->next) {
        const std::size_t count =
                block == global_current_ ? static_cast<std::size_t>(global_top_ - block->slots) : kValueBlockSlots;
        visitor.visit_range(block->slots, count);
        if (block == global_current_) {
            break;
        }
    }
    roots_.visit_all(visitor);
}

void gc_sweep_unmarked(GcHeap *heap); // defined below

namespace {

class MarkingVisitor final : public GcRootVisitor {
public:
    explicit MarkingVisitor(GcHeap &heap) noexcept : heap_(&heap) {}

    void visit(JsValue *value) noexcept override {
        if (value) {
            gc_mark_value(heap_, *value);
        }
    }

private:
    GcHeap *heap_ = nullptr;
};

void gc_mark_protected_new_objects(GcHeap *heap) noexcept {
    if (!heap) {
        return;
    }
    for (GcHeader *obj = heap->head; obj; obj = obj->next) {
        if (obj->first_collect_protected) {
            gc_mark_obj(heap, obj);
        }
    }
}

} // namespace

GcCollectStats GcHeap::collect() {
    std::size_t before = bytes;
    live_mark = flip_mark(live_mark);
    MarkingVisitor visitor(*this);
    visit_roots(visitor);
    gc_mark_protected_new_objects(this);
    gc_sweep_unmarked(this);
    threshold = next_threshold(bytes);
    return {bytes, before - bytes};
}

GcHeap::LocalState GcHeap::mark_local() const noexcept { return LocalState{local_current_, local_top_}; }

void GcHeap::restore_local(LocalState state) noexcept {
    if (!state.block) {
        recycle_local_blocks(local_head_);
        local_head_ = nullptr;
        local_current_ = nullptr;
        local_top_ = nullptr;
        local_end_ = nullptr;
        return;
    }
    ValueBlock *released = state.block->next;
    state.block->next = nullptr;
    recycle_local_blocks(released);
    local_current_ = state.block;
    local_top_ = state.top;
    local_end_ = state.block->slots + kValueBlockSlots;
}

GcHeap::ValueBlock *GcHeap::alloc_value_block() {
    if (!pool_) {
        return nullptr;
    }
    void *mem = pool_->alloc(sizeof(ValueBlock), alignof(ValueBlock));
    if (!mem) {
        return nullptr;
    }
    auto *block = new (mem) ValueBlock();
    reset_block(block);
    return block;
}

GcHeap::ValueBlock *GcHeap::acquire_local_block() {
    ValueBlock *block = local_free_;
    if (block) {
        local_free_ = block->next;
        block->next = nullptr;
        reset_block(block);
        return block;
    }
    return alloc_value_block();
}

GcHeap::ValueBlock *GcHeap::acquire_global_block() {
    ValueBlock *block = alloc_value_block();
    if (block) {
        block->next = nullptr;
    }
    return block;
}

void GcHeap::reset_block(ValueBlock *block) noexcept {
    if (!block) {
        return;
    }
    block->next = nullptr;
    for (auto &slot: block->slots) {
        slot = fiber::script::JsValue::make_undefined();
    }
}

void GcHeap::recycle_local_blocks(ValueBlock *first) noexcept {
    if (!first) {
        return;
    }
    ValueBlock *tail = first;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = local_free_;
    local_free_ = first;
}

GcHeap::LocalMark::LocalMark(GcHeap &heap) noexcept : heap_(&heap), state_(heap.mark_local()) {}

GcHeap::LocalMark::LocalMark(LocalMark &&other) noexcept : heap_(other.heap_), state_(other.state_) {
    other.heap_ = nullptr;
    other.state_ = {};
}

GcHeap::LocalMark &GcHeap::LocalMark::operator=(LocalMark &&other) noexcept {
    if (this != &other) {
        reset();
        heap_ = other.heap_;
        state_ = other.state_;
        other.heap_ = nullptr;
        other.state_ = {};
    }
    return *this;
}

GcHeap::LocalMark::~LocalMark() { reset(); }

void GcHeap::LocalMark::reset() noexcept {
    if (heap_) {
        heap_->restore_local(state_);
    }
    heap_ = nullptr;
    state_ = {};
}

GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) noexcept {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->len = len;
    str->encoding = GcStringEncoding::Byte;
    str->hash = 0;
    str->hash_valid = false;
    str->data8 = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(str);
        return nullptr;
    }
    if (len > 0) {
        str->data8 =
                static_cast<std::uint8_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Byte)));
        if (!str->data8) {
            heap->alloc.free(str);
            return nullptr;
        }
        std::memcpy(str->data8, data, len);
        str->data8[len] = 0;
    }
    gc_link(heap, hdr);
    return str;
}

GcString *gc_new_string_bytes_uninit(GcHeap *heap, std::size_t len) noexcept {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->len = len;
    str->encoding = GcStringEncoding::Byte;
    str->hash = 0;
    str->hash_valid = false;
    str->data8 = nullptr;
    if (len > 0) {
        str->data8 =
                static_cast<std::uint8_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Byte)));
        if (!str->data8) {
            heap->alloc.free(str);
            return nullptr;
        }
        str->data8[len] = 0;
    }
    gc_link(heap, hdr);
    return str;
}

GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len) noexcept {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->len = len;
    str->encoding = GcStringEncoding::Utf16;
    str->hash = 0;
    str->hash_valid = false;
    str->data16 = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(str);
        return nullptr;
    }
    if (len > 0) {
        str->data16 = static_cast<char16_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Utf16)));
        if (!str->data16) {
            heap->alloc.free(str);
            return nullptr;
        }
        std::memcpy(str->data16, data, sizeof(char16_t) * len);
        str->data16[len] = 0;
    }
    gc_link(heap, hdr);
    return str;
}

GcString *gc_new_string_utf16_uninit(GcHeap *heap, std::size_t len) noexcept {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->len = len;
    str->encoding = GcStringEncoding::Utf16;
    str->hash = 0;
    str->hash_valid = false;
    str->data16 = nullptr;
    if (len > 0) {
        str->data16 = static_cast<char16_t *>(gc_alloc_extra(heap, string_storage_bytes(len, GcStringEncoding::Utf16)));
        if (!str->data16) {
            heap->alloc.free(str);
            return nullptr;
        }
        str->data16[len] = 0;
    }
    gc_link(heap, hdr);
    return str;
}

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len) noexcept {
    if (!heap || (len > 0 && !data)) {
        return nullptr;
    }
    fiber::json::Utf8ScanResult scan;
    if (!fiber::json::utf8_scan(data, len, scan)) {
        return nullptr;
    }
    if (scan.all_byte) {
        GcString *str = gc_new_string_bytes_uninit(heap, scan.utf16_len);
        if (!str) {
            return nullptr;
        }
        bool written = fiber::json::utf8_write_bytes(data, len, str->data8, str->len);
        FIBER_ASSERT(written);
        (void) written;
        return str;
    }
    GcString *str = gc_new_string_utf16_uninit(heap, scan.utf16_len);
    if (!str) {
        return nullptr;
    }
    bool written = fiber::json::utf8_write_utf16(data, len, str->data16, str->len);
    FIBER_ASSERT(written);
    (void) written;
    return str;
}

bool gc_string_to_utf8(const GcString *str, std::string &out) {
    out.clear();
    if (!str) {
        return false;
    }
    if (str->len == 0) {
        return true;
    }
    if (str->encoding == GcStringEncoding::Byte) {
        std::size_t extra = 0;
        for (std::size_t i = 0; i < str->len; ++i) {
            if (str->data8[i] >= 0x80) {
                extra += 1;
            }
        }
        if (extra == 0) {
            out.assign(reinterpret_cast<const char *>(str->data8), str->len);
            return true;
        }
        out.resize(str->len + extra);
        char *dst = out.data();
        for (std::size_t i = 0; i < str->len; ++i) {
            std::uint8_t byte = str->data8[i];
            if (byte < 0x80) {
                *dst++ = static_cast<char>(byte);
            } else {
                *dst++ = static_cast<char>(0xC0 | (byte >> 6));
                *dst++ = static_cast<char>(0x80 | (byte & 0x3F));
            }
        }
        return true;
    }
    std::size_t out_len = 0;
    for (std::size_t i = 0; i < str->len; ++i) {
        char16_t unit = str->data16[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 >= str->len) {
                return false;
            }
            char16_t low = str->data16[i + 1];
            if (low < 0xDC00 || low > 0xDFFF) {
                return false;
            }
            out_len += 4;
            i += 1;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
        if (unit < 0x80) {
            out_len += 1;
        } else if (unit < 0x800) {
            out_len += 2;
        } else {
            out_len += 3;
        }
    }
    out.resize(out_len);
    char *dst = out.data();
    for (std::size_t i = 0; i < str->len; ++i) {
        char16_t unit = str->data16[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 >= str->len) {
                return false;
            }
            char16_t low = str->data16[i + 1];
            if (low < 0xDC00 || low > 0xDFFF) {
                return false;
            }
            std::uint32_t codepoint = 0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
                                      (static_cast<std::uint32_t>(low) - 0xDC00);
            *dst++ = static_cast<char>(0xF0 | (codepoint >> 18));
            *dst++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            *dst++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            *dst++ = static_cast<char>(0x80 | (codepoint & 0x3F));
            i += 1;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
        std::uint32_t codepoint = unit;
        if (codepoint < 0x80) {
            *dst++ = static_cast<char>(codepoint);
        } else if (codepoint < 0x800) {
            *dst++ = static_cast<char>(0xC0 | (codepoint >> 6));
            *dst++ = static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            *dst++ = static_cast<char>(0xE0 | (codepoint >> 12));
            *dst++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            *dst++ = static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    }
    return true;
}

GcBinary *gc_new_binary(GcHeap *heap, const std::uint8_t *data, std::size_t len) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcBinary), GcKind::Binary);
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
    auto *hdr = gc_alloc_raw(heap, sizeof(GcArray), GcKind::Array);
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

GcObject *gc_new_object(GcHeap *heap, std::size_t capacity) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcObject), GcKind::Object);
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

namespace {

GcException *gc_new_exception_unchecked(GcHeap *heap, std::int64_t position, GcString *name, GcString *message,
                                        const JsValue &meta) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcException), GcKind::Exception);
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
    GcHeap::LocalMark mark(*heap);
    ValueHandle name_root = heap->local_value();
    ValueHandle message_root = heap->local_value();
    ValueHandle meta_root = heap->local_value();
    if (!name_root || !message_root || !meta_root) {
        return nullptr;
    }
    *name_root = name ? js_make_heap_ref(&name->hdr, JsHeapKind::String) : JsValue::make_undefined();
    *message_root = message ? js_make_heap_ref(&message->hdr, JsHeapKind::String) : JsValue::make_undefined();
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
        *name_root = js_make_heap_ref(&name_str->hdr, JsHeapKind::String);
    }
    if (message || message_len > 0) {
        if (!message && message_len > 0) {
            return nullptr;
        }
        message_str = gc_new_string(heap, message ? message : "", message_len);
        if (!message_str) {
            return nullptr;
        }
        *message_root = js_make_heap_ref(&message_str->hdr, JsHeapKind::String);
    }
    return gc_new_exception_unchecked(heap, position, name_str, message_str, *meta_root);
}

GcException *gc_new_exception(GcHeap *heap, std::int64_t position, const char *name, std::size_t name_len,
                              const char *message, std::size_t message_len) {
    return gc_new_exception(heap, position, name, name_len, message, message_len, JsValue::make_undefined());
}

GcIterator *gc_new_array_iterator(GcHeap *heap, GcArray *array, GcIteratorMode mode) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcIterator), GcKind::Iterator);
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
    auto *hdr = gc_alloc_raw(heap, sizeof(GcIterator), GcKind::Iterator);
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
    if (!iter) {
        return false;
    }
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

bool gc_object_reserve(GcHeap *heap, GcObject *obj, std::size_t expected) {
    if (!heap || !obj) {
        return false;
    }
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

const GcObjectEntry *gc_object_entry_at(const GcObject *obj, std::size_t index) {
    if (!obj || index >= obj->size) {
        return nullptr;
    }
    std::size_t current = 0;
    int32_t cursor = obj->head;
    while (cursor != -1) {
        if (current == index) {
            return &obj->entries[cursor];
        }
        cursor = obj->entries[cursor].next_order;
        current += 1;
    }
    return nullptr;
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

} // namespace fiber::script
