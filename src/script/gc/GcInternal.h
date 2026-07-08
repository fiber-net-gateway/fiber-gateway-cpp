//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_SCRIPT_GC_INTERNAL_H
#define FIBER_SCRIPT_GC_INTERNAL_H

#include "../JsGc.h"

#include <cstddef>
#include <cstdint>

namespace fiber::script::gc_detail {

inline constexpr std::size_t kValueBlockSlots = 8;

std::size_t saturating_add(std::size_t lhs, std::size_t rhs);
std::size_t next_threshold(std::size_t live_bytes);
GcMark flip_mark(GcMark mark);

std::size_t string_storage_bytes(std::size_t len, GcStringEncoding encoding);
std::size_t array_storage_bytes(std::size_t capacity);
std::size_t entry_storage_bytes(std::size_t capacity);
std::size_t bucket_storage_bytes(std::size_t bucket_count);
std::size_t bucket_count_for_entries(std::size_t entry_capacity);

void gc_account_add(GcHeap *heap, std::size_t bytes);
void gc_account_sub(GcHeap *heap, std::size_t bytes);
void *gc_alloc_extra(GcHeap *heap, std::size_t bytes);
void gc_free_extra(GcHeap *heap, void *ptr, std::size_t bytes);

std::uint64_t string_hash(const GcString *str);
bool string_equals(const GcString *lhs, const GcString *rhs);
std::int32_t find_entry_index(const GcObject *obj, const GcString *key, std::uint64_t hash);
bool rehash_buckets(GcHeap *heap, GcObject *obj, std::size_t new_bucket_count);
bool grow_entries(GcHeap *heap, GcObject *obj, std::size_t new_capacity);
std::int32_t allocate_entry(GcObject *obj);

JsValue make_heap_string_value(GcString *str);
bool build_entry_array(GcHeap *heap, const JsValue &key, const JsValue &value, JsValue &out);
bool build_object_snapshot(GcHeap *heap, GcIterator *iter, const GcObject *obj);

GcHeader *gc_alloc_raw(GcHeap *heap, std::size_t size, GcKind kind);
void gc_link(GcHeap *heap, GcHeader *hdr);
void gc_mark_value(GcHeap *heap, const JsValue &value);
void gc_mark_obj(GcHeap *heap, GcHeader *obj);
void gc_free_obj(GcHeap *heap, GcHeader *obj);
void gc_sweep_unmarked(GcHeap *heap);

} // namespace fiber::script::gc_detail

#endif // FIBER_SCRIPT_GC_INTERNAL_H
