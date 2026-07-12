//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include "../../common/Assert.h"

#include <limits>
#include <utility>

namespace fiber::script {

using namespace gc_detail;

GcHeap::GcHeap() : pool_(&owned_pool_) {}

GcHeap::GcHeap(fiber::mem::BufPool &pool) : pool_(&pool) {}

GcHeap::~GcHeap() {
    while (head) {
        GcHeader *obj = head;
        head = obj->next;
        gc_free_obj(this, obj);
    }
    gc_string_intern_free_table(this);
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
    for (GcHeader *obj = heap->head; obj; obj = obj->next) {
        if (obj->first_collect_protected) {
            gc_mark_obj(heap, obj);
        }
    }
}

} // namespace

GcCollectStats GcHeap::collect() {
    if (no_gc_depth_ != 0 || collecting_) {
        gc_pending_ = true;
        return {bytes, 0};
    }

    std::size_t before = bytes;
    collecting_ = true;
    gc_pending_ = false;
    live_mark = flip_mark(live_mark);
    MarkingVisitor visitor(*this);
    visit_roots(visitor);
    gc_mark_protected_new_objects(this);
    gc_sweep_unmarked(this);
    threshold = next_threshold(bytes);
    collecting_ = false;
    return {bytes, before - bytes};
}

void GcHeap::maybe_collect_for_alloc(std::size_t alloc_bytes) {
    if (!threshold || saturating_add(bytes, alloc_bytes) < threshold) {
        return;
    }
    if (no_gc_depth_ != 0 || collecting_) {
        gc_pending_ = true;
        return;
    }
    collect();
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

void GcHeap::enter_no_gc() noexcept {
    FIBER_ASSERT(no_gc_depth_ != std::numeric_limits<std::uint32_t>::max());
    no_gc_depth_ += 1;
}

void GcHeap::leave_no_gc(bool defer_collect) noexcept {
    FIBER_ASSERT(no_gc_depth_ != 0);
    no_gc_depth_ -= 1;
    if (no_gc_depth_ != 0) {
        return;
    }
    if (defer_collect) {
        // The region's allocations are expected to stay rooted, so a collection
        // here would reclaim nothing. Drop the pending request and let the next
        // threshold-driven allocation re-evaluate.
        gc_pending_ = false;
        return;
    }
    if (gc_pending_ && !collecting_) {
        collect();
    }
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

GcHeap::NoGcScope::NoGcScope(GcHeap &heap, bool defer_collect) noexcept : heap_(&heap), defer_collect_(defer_collect) {
    heap_->enter_no_gc();
}

GcHeap::NoGcScope::NoGcScope(NoGcScope &&other) noexcept : heap_(other.heap_), defer_collect_(other.defer_collect_) {
    other.heap_ = nullptr;
}

GcHeap::NoGcScope &GcHeap::NoGcScope::operator=(NoGcScope &&other) noexcept {
    if (this != &other) {
        reset();
        heap_ = other.heap_;
        defer_collect_ = other.defer_collect_;
        other.heap_ = nullptr;
    }
    return *this;
}

GcHeap::NoGcScope::~NoGcScope() { reset(); }

void GcHeap::NoGcScope::reset() noexcept {
    if (heap_) {
        GcHeap *heap = heap_;
        bool defer = defer_collect_;
        heap_ = nullptr;
        heap->leave_no_gc(defer);
    }
}

} // namespace fiber::script
