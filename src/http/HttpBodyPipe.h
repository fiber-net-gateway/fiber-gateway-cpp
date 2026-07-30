#ifndef FIBER_HTTP_BODY_PIPE_H
#define FIBER_HTTP_BODY_PIPE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::http {

inline constexpr std::size_t kDefaultBodyPipeBufferSize = 64 * 1024;
inline constexpr std::size_t kDefaultBodyPipeLowWater = 48 * 1024;

struct HttpBodyPipeOptions {
    std::size_t buffer_size = kDefaultBodyPipeBufferSize;
    std::size_t low_water = kDefaultBodyPipeLowWater;
    std::chrono::milliseconds read_timeout{60000};
    std::chrono::milliseconds write_timeout{60000};
};

enum class HttpBodyPipePhase : std::uint8_t {
    Validate,
    Read,
    Write,
};

struct HttpBodyPipeError {
    common::IoErr code = common::IoErr::None;
    HttpBodyPipePhase phase = HttpBodyPipePhase::Validate;
};

struct HttpBodyPipeStats {
    std::size_t bytes_read = 0;
    std::size_t bytes_written = 0;
    std::size_t peak_buffered_bytes = 0;
    std::uint64_t read_calls = 0;
    std::uint64_t write_calls = 0;
};

using HttpBodyPipeResult = std::expected<HttpBodyPipeStats, HttpBodyPipeError>;

class HttpBodyPipeReader {
public:
    using ReadFn = async::Task<common::IoResult<mem::IoBufChain>> (*)(void *context, std::size_t max_bytes,
                                                                      std::chrono::milliseconds timeout) noexcept;
    using AbortFn = common::IoResult<void> (*)(void *context, common::IoErr reason) noexcept;

    HttpBodyPipeReader() noexcept = default;
    HttpBodyPipeReader(void *context, ReadFn read, AbortFn abort) noexcept :
        context_(context), read_(read), abort_(abort) {}

    [[nodiscard]] bool valid() const noexcept { return context_ != nullptr && read_ != nullptr && abort_ != nullptr; }

    async::Task<common::IoResult<mem::IoBufChain>> read(std::size_t max_bytes,
                                                        std::chrono::milliseconds timeout) const noexcept {
        return read_(context_, max_bytes, timeout);
    }

    common::IoResult<void> abort(common::IoErr reason) const noexcept { return abort_(context_, reason); }

private:
    void *context_ = nullptr;
    ReadFn read_ = nullptr;
    AbortFn abort_ = nullptr;
};

class HttpBodyPipeWriter {
public:
    using WriteFn = async::Task<common::IoResult<std::size_t>> (*)(void *context, mem::IoBufChain &buffer,
                                                                   std::chrono::milliseconds timeout) noexcept;
    using AbortFn = common::IoResult<void> (*)(void *context, common::IoErr reason) noexcept;

    HttpBodyPipeWriter() noexcept = default;
    HttpBodyPipeWriter(void *context, WriteFn write, AbortFn abort) noexcept :
        context_(context), write_(write), abort_(abort) {}

    [[nodiscard]] bool valid() const noexcept { return context_ != nullptr && write_ != nullptr && abort_ != nullptr; }

    async::Task<common::IoResult<std::size_t>> write(mem::IoBufChain &buffer,
                                                     std::chrono::milliseconds timeout) const noexcept {
        return write_(context_, buffer, timeout);
    }

    common::IoResult<void> abort(common::IoErr reason) const noexcept { return abort_(context_, reason); }

private:
    void *context_ = nullptr;
    WriteFn write_ = nullptr;
    AbortFn abort_ = nullptr;
};

template<typename T>
HttpBodyPipeReader make_http_body_pipe_reader(T &source) noexcept {
    return HttpBodyPipeReader{
            &source,
            [](void *context, std::size_t max_bytes,
               std::chrono::milliseconds timeout) noexcept -> async::Task<common::IoResult<mem::IoBufChain>> {
                return static_cast<T *>(context)->read_body(max_bytes, timeout);
            },
            [](void *context, common::IoErr reason) noexcept -> common::IoResult<void> {
                return static_cast<T *>(context)->abort(reason);
            },
    };
}

template<typename T>
HttpBodyPipeWriter make_http_body_pipe_writer(T &sink) noexcept {
    return HttpBodyPipeWriter{
            &sink,
            [](void *context, mem::IoBufChain &buffer,
               std::chrono::milliseconds timeout) noexcept -> async::Task<common::IoResult<std::size_t>> {
                return static_cast<T *>(context)->write(buffer, timeout);
            },
            [](void *context, common::IoErr reason) noexcept -> common::IoResult<void> {
                return static_cast<T *>(context)->abort(reason);
            },
    };
}

async::Task<HttpBodyPipeResult> pipe_http_body(HttpBodyPipeReader source, HttpBodyPipeWriter sink,
                                               mem::IoBufNodePool &node_pool,
                                               const HttpBodyPipeOptions &options) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_BODY_PIPE_H
