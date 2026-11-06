#ifndef FIBER_HTTP_SCRIPT_CONST_PACKAGE_H
#define FIBER_HTTP_SCRIPT_CONST_PACKAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../script/Library.h"

namespace fiber::http_script {

enum class ConstType : std::uint8_t {
    Path = 0,
    Query,
    Header,
    Cookie,
    Count,
};

// Constant-key identity, shared by the package (compile time) and ScriptExchangeCtx
// (request-time resolution). This pair is the single source of truth for normalization
// and hashing — neither side may reimplement it locally.
//
// Normalization is Java-compatible: ASCII lowercase plus '-' -> '_'.

// FNV-1a 64 over the normalized bytes of `name`.
[[nodiscard]] std::uint64_t const_name_hash(std::string_view name) noexcept;

// Compares an already-normalized package key against an arbitrary probe (request data),
// folding the probe through the same normalization.
[[nodiscard]] bool const_name_equal(std::string_view normalized, std::string_view probe) noexcept;

// Maximum constant-name length accepted by ConstPackage::Builder::add_constant. The input
// and its normalized form have the same length (normalization is byte-for-byte), which is
// what keeps the builder allocation-free. Longer names are rejected with nullptr and
// surface as a compile-time "constant not found" error.
inline constexpr std::size_t kMaxConstNameLength = 256;

// Immutable, shareable set of script constants for one configuration snapshot. Compilation
// copies each constant's HostCallable into the script with the package's Ref as userdata;
// at request time the callable forwards (type, normalized name, precomputed hash) to
// ScriptExchangeCtx::resolve_constant via frame.attach. Values are never stored here —
// the ctx resolves and memoizes them per request, so scripts compiled against different
// packages (e.g. across a mid-flight config reload) can share one ctx.
//
// Lifetime contract: compiled scripts retain raw Ref pointers into this package, and the
// request-time memo borrows Ref bytes, so a script and its package must outlive every
// ScriptExchangeCtx the script executes against.
class ConstPackage final {
public:
    class Builder;

    ConstPackage(const ConstPackage &) = delete;
    ConstPackage &operator=(const ConstPackage &) = delete;
    ConstPackage(ConstPackage &&) = delete;
    ConstPackage &operator=(ConstPackage &&) = delete;
    ~ConstPackage();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
    struct Ref;
    struct Storage;

    ConstPackage();

    static fiber::script::AbiResult constant_fn(void *userdata,
                                                const fiber::script::Library::HostCallFrame &frame) noexcept;

    std::unique_ptr<Storage> storage_;
};

// Mutable compile-time collector. Constants are deduplicated after Java-compatible
// ASCII lowercase and '-' -> '_' normalization. build() seals and transfers the package;
// the builder cannot be reused afterwards.
//
// The builder never throws (library-wide no-exceptions rule): every allocation failure
// degrades to add_constant returning nullptr or build returning an empty package, which
// the compiler reports as a failed constant resolution. The single allocating step that
// cannot report through a return value (State + package + shared control block) runs in
// the constructor under a bad_alloc catch and leaves an unusable builder.
class ConstPackage::Builder {
public:
    Builder() noexcept;
    ~Builder();

    Builder(const Builder &) = delete;
    Builder &operator=(const Builder &) = delete;
    Builder(Builder &&) = delete;
    Builder &operator=(Builder &&) = delete;

    [[nodiscard]] const fiber::script::Library::HostCallable *add_constant(ConstType type,
                                                                           std::string_view name) noexcept;
    [[nodiscard]] std::shared_ptr<const ConstPackage> build() noexcept;

private:
    struct State;

    std::unique_ptr<State> state_;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_CONST_PACKAGE_H
