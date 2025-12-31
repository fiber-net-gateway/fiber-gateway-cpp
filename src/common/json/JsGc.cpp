//
// Created by dear on 2025/12/30.
//

#include "JsGc.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fiber::json {
namespace {

GcMark flip_mark(GcMark mark) {
    return (mark == GcMark::GcMark_0) ? GcMark::GcMark_1 : GcMark::GcMark_0;
}

struct DecodedString {
    bool is_byte = true;
    std::vector<std::uint8_t> bytes;
    std::vector<char16_t> u16;
};

void append_code_unit(DecodedString &out, char16_t unit) {
    if (out.is_byte && unit <= 0xFF) {
        out.bytes.push_back(static_cast<std::uint8_t>(unit));
        return;
    }
    if (out.is_byte) {
        out.is_byte = false;
        out.u16.reserve(out.bytes.size() + 1);
        for (std::uint8_t byte : out.bytes) {
            out.u16.push_back(static_cast<char16_t>(byte));
        }
        out.bytes.clear();
    }
    out.u16.push_back(unit);
}

void append_codepoint(DecodedString &out, std::uint32_t codepoint) {
    if (codepoint <= 0xFFFF) {
        append_code_unit(out, static_cast<char16_t>(codepoint));
        return;
    }
    std::uint32_t value = codepoint - 0x10000;
    char16_t high = static_cast<char16_t>(0xD800 + (value >> 10));
    char16_t low = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
    append_code_unit(out, high);
    append_code_unit(out, low);
}

bool decode_utf8(const char *data, std::size_t len, DecodedString &out) {
    std::size_t pos = 0;
    while (pos < len) {
        unsigned char ch = static_cast<unsigned char>(data[pos]);
        if (ch < 0x80) {
            append_code_unit(out, static_cast<char16_t>(ch));
            pos += 1;
            continue;
        }
        int needed = 0;
        std::uint32_t code = 0;
        std::uint32_t min_value = 0;
        if ((ch & 0xE0) == 0xC0) {
            needed = 1;
            code = ch & 0x1F;
            min_value = 0x80;
        } else if ((ch & 0xF0) == 0xE0) {
            needed = 2;
            code = ch & 0x0F;
            min_value = 0x800;
        } else if ((ch & 0xF8) == 0xF0) {
            needed = 3;
            code = ch & 0x07;
            min_value = 0x10000;
        } else {
            return false;
        }
        if (pos + static_cast<std::size_t>(needed) >= len) {
            return false;
        }
        for (int idx = 1; idx <= needed; ++idx) {
            unsigned char next = static_cast<unsigned char>(data[pos + idx]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < min_value || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        append_codepoint(out, code);
        pos += static_cast<std::size_t>(needed) + 1;
    }
    return true;
}

void append_utf8(std::string &out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
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
        case JsNodeType::HeapBinary:
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
            if (str->encoding == GcStringEncoding::Utf16) {
                if (str->data16) {
                    heap->alloc.free(str->data16);
                }
            } else if (str->data8) {
                heap->alloc.free(str->data8);
            }
            break;
        }
        case GcKind::Binary: {
            auto *bin = reinterpret_cast<GcBinary *>(obj);
            if (bin->data) {
                heap->alloc.free(bin->data);
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

GcString *gc_new_string_bytes(GcHeap *heap, const std::uint8_t *data, std::size_t len) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->len = len;
    str->encoding = GcStringEncoding::Byte;
    str->data8 = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(str);
        return nullptr;
    }
    if (len > 0) {
        str->data8 = static_cast<std::uint8_t *>(heap->alloc.alloc(len + 1));
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

GcString *gc_new_string_utf16(GcHeap *heap, const char16_t *data, std::size_t len) {
    auto *hdr = gc_alloc_raw(heap, sizeof(GcString), GcKind::String);
    if (!hdr) {
        return nullptr;
    }
    auto *str = reinterpret_cast<GcString *>(hdr);
    str->len = len;
    str->encoding = GcStringEncoding::Utf16;
    str->data16 = nullptr;
    if (len > 0 && !data) {
        heap->alloc.free(str);
        return nullptr;
    }
    if (len > 0) {
        str->data16 = static_cast<char16_t *>(heap->alloc.alloc(sizeof(char16_t) * (len + 1)));
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

GcString *gc_new_string(GcHeap *heap, const char *data, std::size_t len) {
    if (len > 0 && !data) {
        return nullptr;
    }
    DecodedString decoded;
    if (len > 0 && !decode_utf8(data, len, decoded)) {
        return nullptr;
    }
    if (decoded.is_byte) {
        return gc_new_string_bytes(heap, decoded.bytes.data(), decoded.bytes.size());
    }
    return gc_new_string_utf16(heap, decoded.u16.data(), decoded.u16.size());
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
            std::uint32_t codepoint = 0x10000 +
                                      ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
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
        bin->data = static_cast<std::uint8_t *>(heap->alloc.alloc(len));
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
