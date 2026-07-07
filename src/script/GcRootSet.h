//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_SCRIPT_GCROOTSET_H
#define FIBER_SCRIPT_GCROOTSET_H

#include <cstddef>

#include "../common/IntrusiveList.h"
#include "JsValue.h"

namespace fiber::script {

class GcRootVisitor {
public:
    virtual ~GcRootVisitor() = default;
    virtual void visit(JsValue *value) noexcept = 0;

    void visit_range(JsValue *base, std::size_t count) noexcept {
        if (!base || count == 0) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            visit(base + i);
        }
    }
};

class GcRootSource {
public:
    virtual ~GcRootSource() = default;
    virtual void visit_roots(GcRootVisitor &visitor) noexcept = 0;
};

class GcRootSet;

// RAII registration of a GcRootSource with a GcRootSet. Construction pushes the
// guard into the set's intrusive list; destruction erases it. Both are O(1) and
// noexcept — there is no allocation, unlike the std::vector-backed registry this
// replaces.
//
// The guard itself (not GcRootSource) is the intrusive-list owner type: it must
// be standard-layout to satisfy IntrusiveList's static_assert, whereas
// GcRootSource is polymorphic and therefore not standard-layout. The embedded
// hook carries the linkage; a back-pointer dispatches visit_roots.
//
// Not copyable or movable: a linked node must keep a stable address, since the
// list holds pointers to its hook. Hold it as a member or local for the scope of
// the registration.
class GcRootRegistration {
public:
    GcRootRegistration() noexcept = default;
    GcRootRegistration(GcRootSet &set, GcRootSource &source) noexcept;
    ~GcRootRegistration();

    GcRootRegistration(const GcRootRegistration &) = delete;
    GcRootRegistration &operator=(const GcRootRegistration &) = delete;
    GcRootRegistration(GcRootRegistration &&) = delete;
    GcRootRegistration &operator=(GcRootRegistration &&) = delete;

    void reset() noexcept;

    [[nodiscard]] bool linked() const noexcept { return hook_.linked(); }

private:
    friend class GcRootSet;

    fiber::common::IntrusiveListHook hook_{};
    GcRootSet *set_ = nullptr;
    GcRootSource *source_ = nullptr;
};

class GcRootSet {
public:
    GcRootSet() noexcept = default;
    ~GcRootSet();

    GcRootSet(const GcRootSet &) = delete;
    GcRootSet &operator=(const GcRootSet &) = delete;
    GcRootSet(GcRootSet &&) = delete;
    GcRootSet &operator=(GcRootSet &&) = delete;

    void visit_all(GcRootVisitor &visitor) noexcept;

private:
    friend class GcRootRegistration;

    void push(GcRootRegistration &node) noexcept { list_.push_back(node); }
    void erase(GcRootRegistration &node) noexcept { list_.erase(node); }

    fiber::common::IntrusiveList<GcRootRegistration, offsetof(GcRootRegistration, hook_)> list_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_GCROOTSET_H
