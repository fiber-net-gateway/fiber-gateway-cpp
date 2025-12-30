//
// Created by dear on 2025/12/30.
//

#include "JsGc.h"

#include <cstring>
#include <memory>
#include <utility>

namespace fiber::json {
namespace {

GcMark flip_mark(GcMark mark) {
    return (mark == GcMark::GcMark_0) ? GcMark::GcMark_1 : GcMark::GcMark_0;
}

GcHeader *gc_alloc_raw(GcHeap *heap, std::size_t size, GcKind kind) {
    void *mem = heap->alloc.alloc(size);
    if (!mem) {
        return nullptr;
    }
    auto *hdr = static_cast<GcHeader *>(mem);
    hdr->next = nullptr;
    hdr->mark_ = flip_mark(heap->live_mark);
    hdr->kind = kind;
    hdr->size_ = static_cast<std::uint32_t>(size);
    return hdr;
}

void gc_link(GcHeap *heap, GcHeader *hdr) {
    hdr->next = heap->head;
    heap->head = hdr;
    heap->bytes += hdr->size_;
}

void gc_mark_obj(GcHeap *heap, GcHeader *obj);

void gc_mark_value(GcHeap *heap, const JsValue &value) {
    switch (value.type_) {
        case JsNodeType::HeapString:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Exception:
        case JsNodeType::Interator:
            if (value.gc) {
                gc_mark_obj(heap, value.gc);
            }
            break;
        default:
            break;
    }
}

void gc_mark_obj(GcHeap *heap, GcHeader *obj) {
    if (!obj || obj->mark_ == heap->live_mark) {
        return;
    }
    obj->mark_ = heap->live_mark;
    switch (obj->kind) {
        case GcKind::String:
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
            for (std::size_t i = 0; i < objv->size; ++i) {
                if (objv->entries[i].key) {
                    gc_mark_obj(heap, &objv->entries[i].key->hdr);
                }
                gc_mark_value(heap, objv->entries[i].value);
            }
            break;
        }
        case GcKind::Exception:
        case GcKind::Iterator:
            break;
    }
}

void gc_free_obj(GcHeap *heap, GcHeader *obj) {
    switch (obj->kind) {
        case GcKind::String: {
            auto *str = reinterpret_cast<GcString *>(obj);
            if (str->data) {
                heap->alloc.free(str->data);
            }
            break;
        }
        case GcKind::Array: {
            auto *arr = reinterpret_cast<GcArray *>(obj);
            if (arr->elems) {
                for (std::size_t i = 0; i < arr->capacity; ++i) {
                    std::destroy_at(&arr->elems[i]);
                }
                heap->alloc.free(arr->elems);
            }
            break;
        }
        case GcKind::Object: {
            auto *objv = reinterpret_cast<GcObject *>(obj);
            if (objv->entries) {
                for (std::size_t i = 0; i < objv->capacity; ++i) {
                    std::destroy_at(&objv->entries[i].value);
                }
                heap->alloc.free(objv->entries);
            }
            break;
        }
        case GcKind::Exception:
        case GcKind::Iterator:
            break;
    }
    heap->bytes -= obj->size_;
    heap->alloc.free(obj);
}

} // namespace

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->len = len;
    str->data = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(str);
        return nullptr;
    }
    if (len > 0) {
        str->data = static_cast<char *>(heap->alloc.alloc(len + 1));
        if (!str->data) {
            heap->alloc.free(str);
            return nullptr;
        }
        std::memcpy(str->data, data, len);
        str->data[len] = '\0';
    }
    gc_link(heap, hdr);
    return str;
}

GcArray *gc_new_array(GcHeap *heap, std::size_t capacity) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcArray), GcKind::Array);
    if (!hdr) {
        return nullptr;
    }
    auto *arr = reinterpret_cast<GcArray *>(hdr);
    arr->size = 0;
    arr->capacity = capacity;
    arr->elems = nullptr;
    if (capacity > 0) {
        arr->elems = static_cast<JsValue *>(heap->alloc.alloc(sizeof(JsValue) * capacity));
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

GcObject *gc_new_object(GcHeap *heap, std::size_t capacity) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcObject), GcKind::Object);
    if (!hdr) {
        return nullptr;
    }
    auto *obj = reinterpret_cast<GcObject *>(hdr);
    obj->size = 0;
    obj->capacity = capacity;
    obj->entries = nullptr;
    if (capacity > 0) {
        obj->entries = static_cast<GcObjectEntry *>(heap->alloc.alloc(sizeof(GcObjectEntry) * capacity));
        if (!obj->entries) {
            heap->alloc.free(obj);
            return nullptr;
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            obj->entries[i].key = nullptr;
            std::construct_at(&obj->entries[i].value);
        }
    }
    gc_link(heap, hdr);
    return obj;
}

void gc_collect(GcHeap *heap, JsValue **roots, std::size_t root_count) {
    heap->live_mark = flip_mark(heap->live_mark);
    for (std::size_t i = 0; i < root_count; ++i) {
        gc_mark_value(heap, *roots[i]);
    }
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

void GcRootSet::add_global(JsValue *value) {
    if (value) {
        globals_.push_back(value);
    }
}

void GcRootSet::remove_global(JsValue *value) {
    for (std::size_t i = 0; i < globals_.size(); ++i) {
        if (globals_[i] == value) {
            globals_[i] = globals_.back();
            globals_.pop_back();
            return;
        }
    }
}

void GcRootSet::push_frame() {
    frames_.push_back(stack_.size());
}

void GcRootSet::pop_frame() {
    if (frames_.empty()) {
        return;
    }
    std::size_t size = frames_.back();
    frames_.pop_back();
    if (size < stack_.size()) {
        stack_.resize(size);
    }
}

void GcRootSet::add_stack_root(JsValue *value) {
    if (value) {
        stack_.push_back(value);
    }
}

void GcRootSet::add_temp_root(JsValue *value) {
    if (value) {
        temps_.push_back(value);
    }
}

void GcRootSet::remove_temp_root(JsValue *value) {
    for (std::size_t i = 0; i < temps_.size(); ++i) {
        if (temps_[i] == value) {
            temps_[i] = temps_.back();
            temps_.pop_back();
            return;
        }
    }
}

void GcRootSet::collect(GcHeap &heap) {
    std::vector<JsValue *> roots;
    roots.reserve(globals_.size() + stack_.size() + temps_.size());
    roots.insert(roots.end(), globals_.begin(), globals_.end());
    roots.insert(roots.end(), stack_.begin(), stack_.end());
    roots.insert(roots.end(), temps_.begin(), temps_.end());
    gc_collect(&heap, roots.data(), roots.size());
}

GcRootHandle::GcRootHandle(GcRootSet &roots, JsValue *value)
    : roots_(&roots), value_(value) {
    if (roots_ && value_) {
        roots_->add_temp_root(value_);
    }
}

GcRootHandle::GcRootHandle(GcRootHandle &&other) noexcept
    : roots_(other.roots_), value_(other.value_) {
    other.roots_ = nullptr;
    other.value_ = nullptr;
}

GcRootHandle &GcRootHandle::operator=(GcRootHandle &&other) noexcept {
    if (this != &other) {
        reset();
        roots_ = other.roots_;
        value_ = other.value_;
        other.roots_ = nullptr;
        other.value_ = nullptr;
    }
    return *this;
}

GcRootHandle::~GcRootHandle() {
    reset();
}

void GcRootHandle::reset() {
    if (roots_ && value_) {
        roots_->remove_temp_root(value_);
    }
    roots_ = nullptr;
    value_ = nullptr;
}

} // namespace fiber::json
