#include <fiber/http/Http2Outbound.h>

#include <cstring>

namespace fiber::http {

bool Http2OutboundEncodeTarget::empty() const noexcept { return chain_.readable_bytes() == 0; }

std::size_t Http2OutboundEncodeTarget::total_bytes() const noexcept { return chain_.readable_bytes(); }

common::IoErr Http2OutboundEncodeTarget::append_copy(const void *src, std::size_t bytes) noexcept {
    if (!src || bytes == 0) {
        return common::IoErr::Invalid;
    }

    mem::IoBuf buf = mem::IoBuf::allocate(bytes);
    if (!buf) {
        return common::IoErr::NoMem;
    }
    std::memcpy(buf.writable_data(), src, bytes);
    buf.commit(bytes);
    return chain_.append(std::move(buf)) ? common::IoErr::None : common::IoErr::NoMem;
}

common::IoErr Http2OutboundEncodeTarget::append_buffer(mem::IoBuf &&buf) noexcept {
    if (!buf || buf.readable() == 0) {
        return common::IoErr::Invalid;
    }
    return chain_.append(std::move(buf)) ? common::IoErr::None : common::IoErr::NoMem;
}

common::IoErr Http2OutboundEncodeTarget::append_chain(mem::IoBufChain &&chain) noexcept {
    if (chain.empty() || chain.readable_bytes() == 0) {
        return common::IoErr::Invalid;
    }

    const std::size_t bytes = chain.readable_bytes();
    return chain.take_prefix(bytes, chain_) ? common::IoErr::None : common::IoErr::NoMem;
}

} // namespace fiber::http
