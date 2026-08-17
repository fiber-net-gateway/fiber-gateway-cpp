#ifndef FIBER_HTTP_SCRIPT_SCRIPT_REQUEST_BODY_H
#define FIBER_HTTP_SCRIPT_SCRIPT_REQUEST_BODY_H

#include <chrono>
#include <cstddef>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "../http/HttpBodyPipe.h"

namespace fiber::http_script {

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

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_SCRIPT_REQUEST_BODY_H
