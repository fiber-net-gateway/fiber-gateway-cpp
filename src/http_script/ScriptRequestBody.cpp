#include <fiber/http_script/ScriptRequestBody.h>

#include <algorithm>
#include <cstring>
#include <expected>

namespace fiber::http_script {

namespace {

constexpr std::size_t kMaxReadBytes = static_cast<std::size_t>(64 * 1024);

} // namespace

void drain_chain_into(mem::IoBufChain &chain, std::uint8_t *dst) noexcept {
    while (mem::IoBuf *node = chain.front()) {
        const std::size_t bytes = node->readable();
        if (bytes == 0) {
            chain.drop_empty_front();
            continue;
        }
        std::memcpy(dst, node->readable_data(), bytes);
        dst += bytes;
        chain.consume_and_compact(bytes);
    }
}

async::Task<common::IoResult<void>> read_body_into(ScriptRequestBody body, std::uint8_t *dst, std::size_t len,
                                                   std::chrono::milliseconds timeout) noexcept {
    std::size_t filled = 0;
    for (;;) {
        const std::size_t remaining = len - filled;
        // One byte past the end, so an overrunning source is caught rather than cut off.
        auto result = co_await body.read(std::min(kMaxReadBytes - 1, remaining) + 1, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        const std::size_t bytes = result->readable_bytes();
        const bool complete = result->complete();
        if (bytes > remaining || (bytes == 0 && !complete)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        drain_chain_into(*result, dst + filled);
        filled += bytes;
        if (complete) {
            break;
        }
    }
    if (filled != len) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return common::IoResult<void>{};
}

} // namespace fiber::http_script
