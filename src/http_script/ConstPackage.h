#ifndef FIBER_HTTP_SCRIPT_CONST_PACKAGE_H
#define FIBER_HTTP_SCRIPT_CONST_PACKAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "../script/Library.h"

namespace fiber::http_script {

enum class ConstType : std::uint8_t {
    Path = 0,
    Query,
    Header,
    Cookie,
    Context,
    Count,
};

using ConstIndex = std::uint32_t;
inline constexpr ConstIndex kInvalidConstIndex = UINT32_MAX;

class ConstPackage final {
public:
    class Builder;

    // Compact immutable entry. index is also the offset in the package-wide entry array;
    // name bytes are owned by the package's stable userdata pool.
    struct Entry {
        const char *name_data = nullptr;
        ConstIndex index = kInvalidConstIndex;
        std::uint32_t name_size = 0;

        [[nodiscard]] std::string_view name() const noexcept { return {name_data, name_size}; }
    };

    ConstPackage(const ConstPackage &) = delete;
    ConstPackage &operator=(const ConstPackage &) = delete;
    ConstPackage(ConstPackage &&) = delete;
    ConstPackage &operator=(ConstPackage &&) = delete;
    ~ConstPackage();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] std::span<const Entry> entries(ConstType type) const noexcept;
    [[nodiscard]] ConstIndex find(ConstType type, std::string_view name) const noexcept;

    // Opaque identity used by ScriptExchangeCtx to reject a compiled script executed
    // against slots prepared from another configuration snapshot.
    [[nodiscard]] const void *identity() const noexcept;

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
class ConstPackage::Builder {
public:
    Builder();
    ~Builder();

    Builder(const Builder &) = delete;
    Builder &operator=(const Builder &) = delete;
    Builder(Builder &&) = delete;
    Builder &operator=(Builder &&) = delete;

    [[nodiscard]] const fiber::script::Library::HostCallable *add_constant(ConstType type, std::string_view name);
    [[nodiscard]] std::shared_ptr<const ConstPackage> build();

private:
    struct State;

    std::unique_ptr<State> state_;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_CONST_PACKAGE_H
