#ifndef FIBER_HTTP_HTTP1_HEADER_PARSE_BUFFER_H
#define FIBER_HTTP_HTTP1_HEADER_PARSE_BUFFER_H

#include <cstddef>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "Http1Parser.h"

namespace fiber::http {

struct Http1HeaderParseBufferOptions {
    std::size_t init_size = 8 * 1024;
    std::size_t large_size = 32 * 1024;
    std::size_t large_num = 4;
};

class Http1HeaderParseBuffer : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http1HeaderParseBuffer(Http1HeaderParseBufferOptions options = {}) noexcept;

    common::IoResult<void> ensure_init() noexcept;
    common::IoResult<void> grow() noexcept;

    template<typename Parser>
    common::IoResult<void> grow_and_replace(Parser &parser) noexcept;

    void reset() noexcept;
    [[nodiscard]] bool can_grow() const noexcept;
    [[nodiscard]] std::size_t growth_count() const noexcept { return growth_count_; }

    [[nodiscard]] mem::IoBuf &buf() noexcept { return buf_; }
    [[nodiscard]] const mem::IoBuf &buf() const noexcept { return buf_; }

    [[nodiscard]] common::IoResult<mem::IoBuf> retain_prefix(std::size_t bytes) const noexcept;
    [[nodiscard]] common::IoResult<mem::IoBuf> retain_suffix() const noexcept;

private:
    [[nodiscard]] std::size_t next_capacity() const noexcept;
    [[nodiscard]] common::IoResult<mem::IoBuf> allocate_next() const noexcept;

    Http1HeaderParseBufferOptions options_{};
    mem::IoBuf buf_{};
    std::size_t growth_count_ = 0;
};

template<typename Parser>
common::IoResult<void> Http1HeaderParseBuffer::grow_and_replace(Parser &parser) noexcept {
    if (!buf_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto next_result = allocate_next();
    if (!next_result) {
        return std::unexpected(next_result.error());
    }

    mem::IoBuf old = buf_;
    mem::IoBuf next = std::move(*next_result);
    ParseCode code = parser.replace_buf_ptr(&old, &next);
    if (code != ParseCode::Ok) {
        return std::unexpected(code == ParseCode::HeaderTooLarge ? common::IoErr::Invalid : common::IoErr::NoMem);
    }

    buf_ = std::move(next);
    ++growth_count_;
    return {};
}

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_HEADER_PARSE_BUFFER_H
