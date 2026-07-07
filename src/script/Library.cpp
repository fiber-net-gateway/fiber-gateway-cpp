#include "Library.h"

namespace fiber::script {

Library::FunctionMatchResult Library::FunctionMatchResult::not_found() noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::NotFound;
    return result;
}

Library::FunctionMatchResult Library::FunctionMatchResult::arity_mismatch() noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::ArityMismatch;
    return result;
}

Library::FunctionMatchResult Library::FunctionMatchResult::ambiguous() noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::Ambiguous;
    return result;
}

Library::FunctionMatchResult Library::FunctionMatchResult::found(const HostCallable *callable,
                                                                 FunctionSignature signature,
                                                                 const fiber::script::JsValue *defaults,
                                                                 std::uint16_t default_count) noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::Found;
    result.callable = callable;
    result.signature = signature;
    result.defaults_to_append = defaults;
    result.default_count = default_count;
    return result;
}

} // namespace fiber::script
