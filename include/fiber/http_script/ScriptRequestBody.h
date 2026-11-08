#ifndef FIBER_HTTP_SCRIPT_SCRIPT_REQUEST_BODY_H
#define FIBER_HTTP_SCRIPT_SCRIPT_REQUEST_BODY_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "../http/HttpBodyPipe.h"

namespace fiber::http_script {

// How long a script waits for the next bytes of a body. It limits each read's idle wait, not the
// whole transfer, like proxy_read_timeout's default.
inline constexpr std::chrono::milliseconds kBodyIdleTimeout{60000};

class ScriptRequestBody {
public:
    using DiscardFn = async::Task<common::IoResult<void>> (*)(void *context,
                                                              std::chrono::milliseconds timeout) noexcept;

    ScriptRequestBody(http::HttpBodyPipeReader reader, void *context, DiscardFn discard) noexcept :
        reader_(reader), context_(context), discard_(discard) {}

    [[nodiscard]] bool valid() const noexcept { return reader_.valid() && context_ != nullptr && discard_ != nullptr; }
    [[nodiscard]] http::HttpBodyPipeReader pipe_reader() const noexcept { return reader_; }

    async::Task<common::IoResult<mem::IoBufChain>>
    read(std::size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const noexcept {
        return reader_.read(max_bytes, timeout);
    }

    async::Task<common::IoResult<void>>
    discard(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const noexcept {
        return discard_(context_, timeout);
    }

    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) const noexcept {
        return reader_.abort(reason);
    }

private:
    http::HttpBodyPipeReader reader_;
    void *context_ = nullptr;
    DiscardFn discard_ = nullptr;
};

template<typename T>
ScriptRequestBody make_script_request_body(T &source) noexcept {
    return ScriptRequestBody{
            http::make_http_body_pipe_reader(source),
            &source,
            [](void *context, std::chrono::milliseconds timeout) noexcept -> async::Task<common::IoResult<void>> {
                return static_cast<T *>(context)->discard_body(timeout);
            },
    };
}

// Moves every readable byte of chain to dst, which must hold chain.readable_bytes().
void drain_chain_into(mem::IoBufChain &chain, std::uint8_t *dst) noexcept;

// Reads a body of exactly len bytes (len > 0) into dst, releasing each read as it lands so
// at most one is held. dst is never written past len, whatever the source delivers: a source
// that overruns len, or completes short of it, fails with IoErr::Invalid.
async::Task<common::IoResult<void>> read_body_into(ScriptRequestBody body, std::uint8_t *dst, std::size_t len,
                                                   std::chrono::milliseconds timeout) noexcept;

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_SCRIPT_REQUEST_BODY_H
