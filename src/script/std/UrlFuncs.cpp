#include <fiber/script/std/UrlFuncs.h>

#include <fiber/script/std/StdLibrary.h>

#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/std/NodeText.h>

#include <fiber/common/util/UrlForm.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::script::std_lib {

namespace {

AbiResult type_error() noexcept { return AbiResult::exception(JsValue::make_exception(ExceptionKind::TypeError)); }

// Aggregates one decoded (key, value) pair into the object rooted at obj_root, mirroring
// Java ObjectNode.has/get/put: missing key -> store the string value; existing array ->
// append; existing scalar -> promote to [old, new]. All values are strings (matching
// Java node.put(k, v)). obj_root is a GC root slot held by the caller for the whole
// parse; this function opens its own LocalMark for transient roots (existing value, the
// promoted array, the new string). Returns false only on OOM.
bool aggregate_pair(GcHeap *heap, ValueHandle obj_root, std::string_view key, std::string_view value) noexcept {
    GcHeap::LocalMark mark(*heap);
    ValueHandle existing = heap->local_value();
    if (!existing) {
        return false;
    }
    *existing = JsValue::make_undefined();
    if (!gc_object_get_key(heap, obj_root, key.data(), key.size(), existing)) {
        return false;
    }

    const JsNodeType existing_type = js_value_type(*existing);
    if (existing_type == JsNodeType::Array) {
        // The array is already stored in the object; its header is stable across push
        // (elems is a separate allocation), so no set_key is needed after appending.
        JsValue item = JsValue::make_string(*heap, value.data(), value.size());
        if (js_value_type(item) != JsNodeType::String) {
            return false;
        }
        return gc_array_push(heap, existing, item);
    }

    if (existing_type != JsNodeType::Undefined) {
        // Promote to a fresh array [existing, new]. The new array is not yet in the
        // object, so it (and the new string) must be held in root slots across the
        // make_string allocation.
        ValueHandle arr_root = heap->local_value();
        ValueHandle item_root = heap->local_value();
        if (!arr_root || !item_root) {
            return false;
        }
        *arr_root = JsValue::make_array(*heap, 2);
        if (js_value_type(*arr_root) != JsNodeType::Array) {
            return false;
        }
        if (!gc_array_push(heap, arr_root, *existing)) {
            return false;
        }
        *item_root = JsValue::make_string(*heap, value.data(), value.size());
        if (js_value_type(*item_root) != JsNodeType::String) {
            return false;
        }
        if (!gc_array_push(heap, arr_root, *item_root)) {
            return false;
        }
        return gc_object_set_key(heap, obj_root, key.data(), key.size(), *arr_root);
    }

    // Not found: store the string value.
    ValueHandle item_root = heap->local_value();
    if (!item_root) {
        return false;
    }
    *item_root = JsValue::make_string(*heap, value.data(), value.size());
    if (js_value_type(*item_root) != JsNodeType::String) {
        return false;
    }
    return gc_object_set_key(heap, obj_root, key.data(), key.size(), *item_root);
}

// Stateful pair source for buildQuery: walks the object in insertion order, expanding
// array-valued fields into one (key, value) pair per element (empty arrays yield none),
// and rendering each value via node_json_to_string (Java JsonUtil.toString). The build
// loop performs only std::string work (no GC allocation), so the input object - rooted in
// the host-call frame - stays valid throughout.
struct QueryPairSource {
    const GcObject *obj;
    const GcObjectEntry *entry;
    const GcArray *arr;
    std::size_t arr_idx;
    std::string key_buf;

    bool operator()(std::string &key, std::string &value) noexcept {
        for (;;) {
            if (arr != nullptr) {
                if (arr_idx < arr->size) {
                    value.clear();
                    node_json_to_string(arr->elems[arr_idx], value);
                    key = key_buf;
                    ++arr_idx;
                    return true;
                }
                arr = nullptr;
                entry = gc_object_next_entry(obj, entry);
                continue;
            }
            if (entry == nullptr) {
                return false;
            }
            if (!entry->occupied || !entry->key) {
                entry = gc_object_next_entry(obj, entry);
                continue;
            }
            gc_string_to_utf8(entry->key, key_buf); // clears + assigns the field key
            const JsValue &v = entry->value;
            if (js_value_type(v) == JsNodeType::Array) {
                arr = js_value_heap_ptr<const GcArray>(v);
                arr_idx = 0;
                continue; // yield first element (or skip if empty) next iteration
            }
            key = key_buf;
            value.clear();
            node_json_to_string(v, value);
            entry = gc_object_next_entry(obj, entry);
            return true;
        }
    }
};

// ---- URL.encodeComponent(value) ----

AbiResult url_encode_component_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                                  Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    std::string_view input;
    if (!string_utf8_view(args.args[0], input)) {
        return type_error();
    }
    std::string out;
    fiber::util::form_encode(input, out);
    JsValue result = JsValue::make_string(frame.runtime, out.data(), out.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

// ---- URL.decodeComponent(value) ----

AbiResult url_decode_component_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                                  Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    std::string_view input;
    if (!string_utf8_view(args.args[0], input)) {
        return type_error();
    }
    auto decoded = fiber::util::form_decode(input);
    if (!decoded) {
        // Malformed percent escape (Java IllegalArgumentException).
        return AbiResult::exception(JsValue::make_exception(ExceptionKind::RangeError));
    }
    JsValue result = JsValue::make_string(frame.runtime, decoded->data(), decoded->size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

// ---- URL.parseQuery(value) ----

AbiResult url_parse_query_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                             Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    std::string_view input;
    if (!string_utf8_view(args.args[0], input)) {
        return type_error();
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }

    // Root the result object for the whole parse: aggregate_pair allocates (make_string,
    // make_array) on each pair, and the object must survive those collections.
    GcHeap::LocalMark mark(*heap);
    ValueHandle obj_root = heap->local_value();
    if (!obj_root) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    *obj_root = JsValue::make_object(*heap, 0);
    if (js_value_type(*obj_root) != JsNodeType::Object) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }

    bool oom = false;
    auto io = fiber::util::form_decode_query(input, [&](std::string_view k, std::string_view v) -> bool {
        if (!aggregate_pair(heap, obj_root, k, v)) {
            oom = true;
            return false;
        }
        return true;
    });
    if (oom) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!io) {
        // Malformed percent escape (Java IllegalArgumentException, caught + rethrown).
        return AbiResult::exception(JsValue::make_exception(ExceptionKind::RangeError));
    }
    return AbiResult::success(*obj_root);
}

// ---- URL.buildQuery(value) ----

AbiResult url_build_query_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                             Library::Arguments args) noexcept {
    // buildQuery has a default (undefined) for the optional value arg, so argc >= 1.
    JsValue arg = (args.args && args.argc >= 1) ? args.args[0] : JsValue::make_undefined();
    JsNodeType t = js_value_type(arg);
    if (t == JsNodeType::Null || t == JsNodeType::Undefined) {
        // Java: null / missing -> return the value as-is.
        return AbiResult::success(arg);
    }
    if (t != JsNodeType::Object) {
        return type_error();
    }
    const GcObject *obj = js_value_heap_ptr<const GcObject>(arg);
    if (obj == nullptr) {
        return type_error();
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    QueryPairSource src{obj, gc_object_first_entry(obj), nullptr, 0, std::string{}};
    std::string out;
    fiber::util::form_build_query(out, src);
    JsValue result = JsValue::make_string(*heap, out.data(), out.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

} // namespace

void register_url_funcs(StdLibrary &lib) {
    lib.register_func("URL.encodeComponent",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &url_encode_component_fn, nullptr, "URL.encodeComponent");
    lib.register_func("URL.decodeComponent",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &url_decode_component_fn, nullptr, "URL.decodeComponent");
    lib.register_func("URL.parseQuery",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &url_parse_query_fn, nullptr, "URL.parseQuery");
    lib.register_func("URL.buildQuery",
                      Library::FunctionSignature{.required_argc = 0, .fixed_argc = 1, .variadic = false},
                      {JsValue::make_undefined()}, &url_build_query_fn, nullptr, "URL.buildQuery");
}

} // namespace fiber::script::std_lib
