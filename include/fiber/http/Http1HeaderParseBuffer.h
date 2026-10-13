#ifndef FIBER_HTTP_HTTP1_HEADER_PARSE_BUFFER_H
#define FIBER_HTTP_HTTP1_HEADER_PARSE_BUFFER_H

#include <cstddef>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "../common/mem/IoBufChain.h"
#include "Http1Parser.h"

namespace fiber::http {

struct Http1HeaderParseBufferOptions {
    std::size_t init_size = static_cast<std::size_t>(8 * 1024);
    std::size_t large_size = static_cast<std::size_t>(32 * 1024);
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

    // View-lifetime tracking. A parsed line's views point exclusively into the
    // buffer storage where that line completed (replace_buf_ptr restages the
    // whole in-flight line into each new storage). Callers take a view (header
    // fields, method/URI/reason views) via note_view(); before a storage is
    // dropped by grow, pin_viewed_storage() keeps it alive for the exchange —
    // but only if it carries views, and at most once per storage. Storages
    // grown through with no completed line carry no views and are freed.
    void note_view() noexcept { has_views_ = true; }
    common::IoResult<void> pin_viewed_storage(mem::IoBufChain &owner) noexcept;

    [[nodiscard]] common::IoResult<mem::IoBuf> retain_suffix() const noexcept;

private:
    [[nodiscard]] std::size_t next_capacity() const noexcept;
    [[nodiscard]] common::IoResult<mem::IoBuf> allocate_next() const noexcept;
    void clear_view_state() noexcept;

    Http1HeaderParseBufferOptions options_{};
    mem::IoBuf buf_{};
    std::size_t growth_count_ = 0;
    bool has_views_ = false;
    bool pinned_ = false;
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
    clear_view_state();
    return {};
}

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_HEADER_PARSE_BUFFER_H
