#ifndef FIBER_SCRIPT_SCRIPT_H
#define FIBER_SCRIPT_SCRIPT_H

#include "Library.h"

namespace fiber::script {

class Script {
public:
    virtual ~Script() = default;

    virtual async::Task<fiber::json::JsValue> exec_async(const fiber::json::JsValue &root,
                                                 void *attach,
                                                 async::IScheduler *scheduler) = 0;

    async::Task<fiber::json::JsValue> exec_async(const fiber::json::JsValue &root, void *attach) {
        return exec_async(root, attach, nullptr);
    }

    virtual fiber::json::JsValue exec_sync(const fiber::json::JsValue &root, void *attach) = 0;

    virtual bool contains_async() const = 0;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_H
