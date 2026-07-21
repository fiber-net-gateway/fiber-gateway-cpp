#ifndef FIBER_CAT_CLIENT_CORE_H
#define FIBER_CAT_CLIENT_CORE_H

#include "CatEncoder.h"

namespace fiber::cat::detail {

class CatClientCore {
public:
    virtual ~CatClientCore() = default;

    CatClientCore(const CatClientCore &) = delete;
    CatClientCore &operator=(const CatClientCore &) = delete;

    [[nodiscard]] virtual ClientEncodeContext encode_context() const noexcept = 0;
    [[nodiscard]] virtual bool accepts_messages() const noexcept = 0;
    virtual void submit_encoded(mem::IoBuf message) noexcept = 0;
    virtual void on_encode_failure(EncodeError error) noexcept = 0;

protected:
    CatClientCore() noexcept = default;
};

} // namespace fiber::cat::detail

#endif // FIBER_CAT_CLIENT_CORE_H
