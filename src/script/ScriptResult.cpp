#include <fiber/script/ScriptResult.h>

#include <string_view>

namespace fiber::script {

std::string_view abort_reason_name(ScriptAbortReason reason) noexcept {
    switch (reason) {
        case ScriptAbortReason::None:
            return "None";
        case ScriptAbortReason::OutOfMemory:
            return "OutOfMemory";
        case ScriptAbortReason::InvalidState:
            return "InvalidState";
        case ScriptAbortReason::InvalidOpcode:
            return "InvalidOpcode";
        case ScriptAbortReason::HostFault:
            return "HostFault";
        case ScriptAbortReason::Timeout:
            return "Timeout";
        case ScriptAbortReason::Cancelled:
            return "Cancelled";
        case ScriptAbortReason::Internal:
            return "Internal";
    }
    return "Internal";
}

} // namespace fiber::script
