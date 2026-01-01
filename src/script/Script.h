#ifndef FIBER_SCRIPT_SCRIPT_H
#define FIBER_SCRIPT_SCRIPT_H

#include <memory>

#include "../common/json/JsNode.h"
#include "async/Task.h"
#include "ir/Compiled.h"

namespace fiber::json {
class GcHeap;
class GcRootSet;
} // namespace fiber::json

namespace fiber::script {

class ScriptRuntime;

class Script {
public:
    Script() = default;
    explicit Script(std::shared_ptr<ir::Compiled> compiled);

    async::Task<fiber::json::JsValue> exec_async(const fiber::json::JsValue &root,
                                                 void *attach,
                                                 async::IScheduler *scheduler,
                                                 ScriptRuntime &runtime);

    async::Task<fiber::json::JsValue> exec_async(const fiber::json::JsValue &root,
                                                 void *attach,
                                                 ScriptRuntime &runtime) {
        return exec_async(root, attach, nullptr, runtime);
    }

    async::Task<fiber::json::JsValue> exec_async(const fiber::json::JsValue &root,
                                                 void *attach,
                                                 async::IScheduler *scheduler,
                                                 fiber::json::GcHeap &heap,
                                                 fiber::json::GcRootSet &roots);

    async::Task<fiber::json::JsValue> exec_async(const fiber::json::JsValue &root,
                                                 void *attach,
                                                 fiber::json::GcHeap &heap,
                                                 fiber::json::GcRootSet &roots) {
        return exec_async(root, attach, nullptr, heap, roots);
    }

    fiber::json::JsValue exec_sync(const fiber::json::JsValue &root,
                                   void *attach,
                                   ScriptRuntime &runtime);

    fiber::json::JsValue exec_sync(const fiber::json::JsValue &root,
                                   void *attach,
                                   fiber::json::GcHeap &heap,
                                   fiber::json::GcRootSet &roots);

    bool contains_async() const;

private:
    std::shared_ptr<ir::Compiled> compiled_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_H
