#include "Runtime.h"

#include <limits>
#include <new>

namespace fiber::script {

namespace {

constexpr std::size_t kMinGcThreshold = 1 << 20;
constexpr std::size_t kValueBlockSlots = 8;

std::size_t next_threshold(std::size_t live_bytes) {
    std::size_t grown = live_bytes;
    if (grown <= (std::numeric_limits<std::size_t>::max() >> 1)) {
        grown *= 2;
    } else {
        grown = std::numeric_limits<std::size_t>::max();
    }
    return grown < kMinGcThreshold ? kMinGcThreshold : grown;
}

} // namespace

ScriptRuntime::ScriptRuntime(fiber::json::GcHeap &heap) : heap_(&heap), pool_(&owned_pool_) {}

ScriptRuntime::ScriptRuntime(fiber::json::GcHeap &heap, fiber::mem::BufPool &pool) : heap_(&heap), pool_(&pool) {}

fiber::json::GcHeap &ScriptRuntime::heap() { return *heap_; }

const fiber::json::GcHeap &ScriptRuntime::heap() const { return *heap_; }

ValueHandle ScriptRuntime::local_value() {
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
    *handle = fiber::json::JsValue::make_undefined();
    return handle;
}

ValueHandle ScriptRuntime::global_value() {
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
    *handle = fiber::json::JsValue::make_undefined();
    return handle;
}

void ScriptRuntime::add_root_source(fiber::json::GcRootSource *source) {
    if (source) {
        root_sources_.push_back(source);
    }
}

void ScriptRuntime::remove_root_source(fiber::json::GcRootSource *source) {
    for (std::size_t i = 0; i < root_sources_.size(); ++i) {
        if (root_sources_[i] == source) {
            root_sources_[i] = root_sources_.back();
            root_sources_.pop_back();
            return;
        }
    }
}

void ScriptRuntime::visit_roots(fiber::json::GcRootVisitor &visitor) noexcept {
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
    for (fiber::json::GcRootSource *source: root_sources_) {
        if (source) {
            source->visit_roots(visitor);
        }
    }
}

bool ScriptRuntime::should_collect(std::size_t next_bytes) const {
    if (!heap_) {
        return false;
    }
    std::size_t used = fiber::json::gc_bytes_used(*heap_);
    std::size_t threshold = fiber::json::gc_threshold(*heap_);
    if (threshold == 0) {
        return false;
    }
    return used + next_bytes >= threshold;
}

void ScriptRuntime::collect_now() {
    if (!heap_) {
        return;
    }
    fiber::json::gc_collect(*heap_, *this);
    fiber::json::gc_set_threshold(*heap_, next_threshold(fiber::json::gc_bytes_used(*heap_)));
}

void ScriptRuntime::maybe_collect(std::size_t next_bytes) {
    if (!should_collect(next_bytes)) {
        return;
    }
    collect_now();
}

ScriptRuntime::LocalState ScriptRuntime::mark_local() const noexcept { return LocalState{local_current_, local_top_}; }

void ScriptRuntime::restore_local(LocalState state) noexcept {
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

ScriptRuntime::ValueBlock *ScriptRuntime::alloc_value_block() {
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

ScriptRuntime::ValueBlock *ScriptRuntime::acquire_local_block() {
    ValueBlock *block = local_free_;
    if (block) {
        local_free_ = block->next;
        block->next = nullptr;
        reset_block(block);
        return block;
    }
    return alloc_value_block();
}

ScriptRuntime::ValueBlock *ScriptRuntime::acquire_global_block() {
    ValueBlock *block = alloc_value_block();
    if (block) {
        block->next = nullptr;
    }
    return block;
}

void ScriptRuntime::reset_block(ValueBlock *block) noexcept {
    if (!block) {
        return;
    }
    block->next = nullptr;
    for (auto &slot: block->slots) {
        slot = fiber::json::JsValue::make_undefined();
    }
}

void ScriptRuntime::recycle_local_blocks(ValueBlock *first) noexcept {
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

ScriptRuntime::LocalMark::LocalMark(ScriptRuntime &runtime) noexcept :
    runtime_(&runtime), state_(runtime.mark_local()) {}

ScriptRuntime::LocalMark::LocalMark(LocalMark &&other) noexcept : runtime_(other.runtime_), state_(other.state_) {
    other.runtime_ = nullptr;
    other.state_ = {};
}

ScriptRuntime::LocalMark &ScriptRuntime::LocalMark::operator=(LocalMark &&other) noexcept {
    if (this != &other) {
        reset();
        runtime_ = other.runtime_;
        state_ = other.state_;
        other.runtime_ = nullptr;
        other.state_ = {};
    }
    return *this;
}

ScriptRuntime::LocalMark::~LocalMark() { reset(); }

void ScriptRuntime::LocalMark::reset() noexcept {
    if (runtime_) {
        runtime_->restore_local(state_);
    }
    runtime_ = nullptr;
    state_ = {};
}

} // namespace fiber::script
