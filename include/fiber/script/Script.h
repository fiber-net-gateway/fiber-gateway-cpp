#ifndef FIBER_SCRIPT_SCRIPT_H
#define FIBER_SCRIPT_SCRIPT_H

#include <memory>

#include "../async/Task.h"
#include "JsValue.h"
#include "ScriptResult.h"
#include "ir/Compiled.h"

namespace fiber::script {

class GcHeap;

class Script {
public:
    Script() = default;
    explicit Script(std::shared_ptr<ir::Compiled> compiled);

    fiber::async::Task<ScriptResult> exec_async(fiber::script::JsValue root, void *attach, fiber::script::GcHeap &heap);

    ScriptResult exec_sync(fiber::script::JsValue root, void *attach, fiber::script::GcHeap &heap) const;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(compiled_); }
    [[nodiscard]] bool contains_async() const noexcept;

private:
    std::shared_ptr<ir::Compiled> compiled_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_H
