#ifndef FIBER_HTTP_SCRIPT_EXCHANGE_CONST_EXTENSION_H
#define FIBER_HTTP_SCRIPT_EXCHANGE_CONST_EXTENSION_H

#include <cstdint>
#include <string_view>

#include "../script/std/StdLibrary.h"

namespace fiber::http_script {

// Resolves the closed set of request/connection constants. Unlike route constants,
// these values are read directly from ScriptExchangeCtx and never consume package slots.
class ExchangeConstExtension final {
public:
    using Library = fiber::script::Library;
    using HostCallable = Library::HostCallable;
    using HostCallFrame = Library::HostCallFrame;

    [[nodiscard]] static const fiber::script::std_lib::StdLibrary::ExtOps &ops() noexcept;

private:
    enum class Field : std::uint8_t {
        ReqUri = 0,
        ReqMethod,
        ReqPath,
        ReqQuery,
        ConnRemoteAddr,
        ConnRemotePort,
        ConnHttpVersion,
        ConnScheme,
        ConnTls,
        Count,
    };

    struct FieldRef {
        Field field = Field::ReqUri;
    };

    struct Table;

    [[nodiscard]] static const Table &table() noexcept;
    [[nodiscard]] static const HostCallable *resolve_constant(std::string_view namespace_name,
                                                              std::string_view key) noexcept;
    [[nodiscard]] static const HostCallable *resolve_constant_op(void *ctx, std::string_view namespace_name,
                                                                 std::string_view key) noexcept;
    static fiber::script::AbiResult constant_fn(void *userdata, const HostCallFrame &frame) noexcept;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_EXCHANGE_CONST_EXTENSION_H
