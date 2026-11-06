#ifndef FIBER_SCRIPT_STD_OOMGUARD_H
#define FIBER_SCRIPT_STD_OOMGUARD_H

#include <new>

#include <fiber/script/ScriptResult.h>

namespace fiber::script::std_lib {

// Runs body() for noexcept host functions that still use throwing std::string
// staging, mapping std::bad_alloc to the script module's graceful OOM abort —
// the same contract the GC-side allocation paths already honor. Only bad_alloc
// is caught: anything else escaping staging allocation is a bug and terminates
// loudly per house policy. The catch handler must not allocate.
// Precedent: compile_template_string (ScriptCompiler.cpp).
template<typename Body>
// NOLINTNEXTLINE(bugprone-exception-escape): only bad_alloc is caught by design — other exceptions escaping staging
// allocation would be a bug that should terminate loudly (house policy; feature/script_noexcept_audit.md §5.2)
AbiResult oom_guard(Body &&body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc &) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
}

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_OOMGUARD_H
