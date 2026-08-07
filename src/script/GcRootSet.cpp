//
// Created by dear on 2025/12/30.
//

#include <fiber/script/GcRootSet.h>

namespace fiber::script {

GcRootRegistration::GcRootRegistration(GcRootSet &set, GcRootSource &source) noexcept : set_(&set), source_(&source) {
    set_->push(*this);
}

GcRootRegistration::~GcRootRegistration() { reset(); }

void GcRootRegistration::reset() noexcept {
    // Only touch the owning set while we are still linked. After the set has
    // orphaned us (see ~GcRootSet), hook_ is unlinked and set_ may dangle — the
    // linked() check reads our own hook and avoids dereferencing a destroyed set.
    if (set_ != nullptr && hook_.linked()) {
        set_->erase(*this);
    }
    set_ = nullptr;
    source_ = nullptr;
}

GcRootSet::~GcRootSet() {
    // Orphan every still-registered guard so a guard destroyed after us does not
    // reach back into this list. erase() flips each hook's in_list to false,
    // which makes the guard's own reset() a safe no-op.
    while (GcRootRegistration *node = list_.front()) {
        list_.erase(*node);
    }
}

void GcRootSet::visit_all(GcRootVisitor &visitor) noexcept {
    for (GcRootRegistration *node = list_.front(); node != nullptr; node = list_.next_of(*node)) {
        if (node->source_ != nullptr) {
            node->source_->visit_roots(visitor);
        }
    }
}

} // namespace fiber::script
