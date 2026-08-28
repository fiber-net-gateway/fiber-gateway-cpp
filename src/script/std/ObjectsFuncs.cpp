#include "script/std/ObjectsFuncs.h"

#include <fiber/script/std/StdLibrary.h>

#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/gc/GcInternal.h>

#include <cstdint>

namespace fiber::script::std_lib {

namespace {

AbiResult type_error() noexcept { return AbiResult::exception(JsValue::make_exception(ExceptionKind::TypeError)); }

// Mirrors Java ObjectsFuncs.firstObj: the primary object argument must be an Object,
// otherwise a catchable TypeError (Java ScriptExecException). Returns the GcObject on
// success, nullptr otherwise.
const GcObject *require_object(const JsValue &value) noexcept {
    if (js_value_type(value) != JsNodeType::Object) {
        return nullptr;
    }
    return js_value_heap_ptr<const GcObject>(value);
}

// Mirrors Java ObjectsFuncs.copyObject (ObjectNode.setAll): only Object sources
// contribute their fields; non-object sources are silently skipped (instanceof
// ObjectNode guard). Fields overwrite existing keys in place and append new ones,
// preserving insertion order. Returns false only on OOM.
bool copy_object(GcHeap *heap, JsValue &target_val, const JsValue &src_val) noexcept {
    if (js_value_type(src_val) != JsNodeType::Object) {
        return true;
    }
    const GcObject *src = js_value_heap_ptr<const GcObject>(src_val);
    GcObject *tgt = js_value_heap_ptr<GcObject>(target_val);
    if (src == nullptr || tgt == nullptr) {
        return true;
    }
    GcHeap::NoGcScope no_gc(*heap);
    if (!gc_object_reserve(heap, tgt, tgt->size + src->size)) {
        return false;
    }
    for (const GcObjectEntry *entry = gc_object_first_entry(src); entry != nullptr;
         entry = gc_object_next_entry(src, entry)) {
        if (!entry->occupied || !entry->key) {
            continue;
        }
        // gc_object_set_heap_key reuses the source's GcString key (no re-materialization).
        if (!gc_object_set_heap_key(heap, ValueHandle(&target_val), entry->key, entry->value)) {
            return false;
        }
    }
    return true;
}

// Builds a fresh array of the object's keys (as strings) or its values, in insertion
// order. Pre-sized to obj->size so pushes never grow under the NoGcScope. Returns false
// on OOM (out left as a non-Array JsValue).
bool collect_object_entries(GcHeap *heap, const GcObject *obj, bool keys, JsValue &out) noexcept {
    GcHeap::NoGcScope no_gc(*heap);
    JsValue result = JsValue::make_array(*heap, obj->size);
    if (js_value_type(result) != JsNodeType::Array) {
        return false;
    }
    for (const GcObjectEntry *entry = gc_object_first_entry(obj); entry != nullptr;
         entry = gc_object_next_entry(obj, entry)) {
        if (!entry->occupied || !entry->key) {
            continue;
        }
        // keys: wrap the source's existing GcString key as a shared HeapRef string (zero
        // allocation). values: copy the entry value as-is.
        JsValue item = keys ? js_make_heap_ref(&entry->key->hdr, GcHeapKind::String) : entry->value;
        if (!gc_array_push(heap, ValueHandle(&result), item)) {
            return false;
        }
    }
    out = result;
    return true;
}

// ---- Object.assign(target, source, ...sources) ----

AbiResult object_assign_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 2) {
        return type_error();
    }
    JsValue target_val = args.args[0];
    if (require_object(target_val) == nullptr) {
        return type_error();
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    for (std::uint32_t i = 1; i < args.argc; ++i) {
        if (!copy_object(heap, target_val, args.args[i])) {
            return AbiResult::abort(ScriptAbortReason::OutOfMemory);
        }
    }
    return AbiResult::success(target_val);
}

// ---- Object.keys(obj) / Object.values(obj) ----

AbiResult object_keys_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    const GcObject *obj = require_object(args.args[0]);
    if (obj == nullptr) {
        return type_error();
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    JsValue result;
    if (!collect_object_entries(heap, obj, /*keys=*/true, result)) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

AbiResult object_values_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    const GcObject *obj = require_object(args.args[0]);
    if (obj == nullptr) {
        return type_error();
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    JsValue result;
    if (!collect_object_entries(heap, obj, /*keys=*/false, result)) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

// ---- Object.deleteProperties(obj, key, ...keys) ----

AbiResult object_delete_properties_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                                      Library::Arguments args) noexcept {
    if (!args.args || args.argc < 2) {
        return type_error();
    }
    JsValue target_val = args.args[0];
    if (require_object(target_val) == nullptr) {
        return type_error();
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    // Java remove() only acts on textual keys (isTextual guard); non-string keys are
    // silently skipped. gc_object_remove returns false for both "not found" and OOM;
    // Java treats a missing key as a no-op, so the return is ignored. Key-materialization
    // OOM is indistinguishable here and accepted as a rare, non-fatal edge.
    for (std::uint32_t i = 1; i < args.argc; ++i) {
        const JsValue &key = args.args[i];
        if (js_value_type(key) != JsNodeType::String) {
            continue;
        }
        (void) gc_object_remove(heap, ValueHandle(&target_val), key);
    }
    return AbiResult::success(target_val);
}

} // namespace

void register_objects_funcs(StdLibrary &lib) {
    lib.register_func("Object.assign",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = true},
                      &object_assign_fn, nullptr, "Object.assign");
    lib.register_func("Object.keys", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &object_keys_fn, nullptr, "Object.keys");
    lib.register_func("Object.values",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &object_values_fn, nullptr, "Object.values");
    lib.register_func("Object.deleteProperties",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = true},
                      &object_delete_properties_fn, nullptr, "Object.deleteProperties");
}

} // namespace fiber::script::std_lib
