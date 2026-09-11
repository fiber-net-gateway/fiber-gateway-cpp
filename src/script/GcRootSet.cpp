//
// Created by dear on 2025/12/30.
//

#include <fiber/script/GcRootSet.h>

namespace fiber::script {

GcRootRegistration::GcRootRegistration(GcRootSet &set, GcRootSource &source) noexcept : source_(&source) {
    set.push(*this);
}

GcRootRegistration::~GcRootRegistration() { reset(); }

void GcRootRegistration::reset() noexcept {
    // The hook unlinks itself without needing the owning set. After ~GcRootSet
    // orphaned us, hook_ is unlinked and this is a no-op even though the set
    // may already be destroyed.
    hook_.unlink_self();
    source_ = nullptr;
}

GcRootSet::~GcRootSet() {
    // Orphan every still-registered guard so a guard destroyed after us does not
    // unlink through this list's dead anchor. Erasing re-links each hook to
    // itself, which makes the guard's own reset() a safe no-op.
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
