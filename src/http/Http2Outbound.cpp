#include <fiber/http/Http2Outbound.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::http {

namespace {

mem::IoBufNodePool &node_pool() noexcept { return event::EventLoop::current().io_buf_node_pool(); }

} // namespace

Http2OutboundEncodeTarget::Http2OutboundEncodeTarget(mem::IoBufChain &chain) noexcept :
    chain_(&chain), base_bytes_(chain.readable_bytes()) {}

Http2OutboundEncodeTarget::~Http2OutboundEncodeTarget() { release_pending(); }

std::size_t Http2OutboundEncodeTarget::total_bytes() const noexcept { return chain_->readable_bytes() - base_bytes_; }

common::IoErr Http2OutboundEncodeTarget::acquire(std::size_t min_bytes, std::size_t capacity_hint, std::uint8_t *&dst,
                                                 std::size_t &len) noexcept {
    FIBER_ASSERT(min_bytes != 0);
    if (pending_ == nullptr) {
        // A slice's view ends at its own bytes, so only a buffer held whole
        // has free space; unique() rules out another view of the same room.
        mem::IoBuf *tail = chain_->back();
        if (tail != nullptr && tail->unique() && tail->writable() >= min_bytes) {
            dst = tail->writable_data();
            len = tail->writable();
            return common::IoErr::None;
        }
        pending_ = node_pool().alloc();
        if (pending_ == nullptr) {
            return common::IoErr::NoMem;
        }
    }
    if (pending_->buf.writable() < min_bytes) {
        pending_->buf = mem::IoBuf::allocate(std::max(min_bytes, capacity_hint));
        if (!pending_->buf) {
            return common::IoErr::NoMem;
        }
    }
    dst = pending_->buf.writable_data();
    len = pending_->buf.writable();
    return common::IoErr::None;
}

void Http2OutboundEncodeTarget::commit(std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    if (pending_ == nullptr) {
        chain_->commit_tailroom(bytes);
        return;
    }
    pending_->buf.commit(bytes);
    const bool linked = chain_->append_node(std::exchange(pending_, nullptr));
    FIBER_ASSERT(linked);
}

common::IoErr Http2OutboundEncodeTarget::append_copy(const void *src, std::size_t bytes) noexcept {
    if (!src || bytes == 0) {
        return common::IoErr::Invalid;
    }

    std::uint8_t *dst = nullptr;
    std::size_t len = 0;
    common::IoErr err = acquire(bytes, bytes, dst, len);
    if (err != common::IoErr::None) {
        return err;
    }
    std::memcpy(dst, src, bytes);
    commit(bytes);
    return common::IoErr::None;
}

common::IoErr Http2OutboundEncodeTarget::append_chain(mem::IoBufChain &&chain) noexcept {
    const std::size_t bytes = chain.readable_bytes();
    if (bytes == 0) {
        return common::IoErr::Invalid;
    }

    // Room acquired but never committed would otherwise land after these bytes.
    release_pending();
    chain.clear_complete();
    return chain.take_prefix(bytes, *chain_) ? common::IoErr::None : common::IoErr::NoMem;
}

void Http2OutboundEncodeTarget::rollback() noexcept {
    release_pending();
    chain_->trim_end(total_bytes());
}

void Http2OutboundEncodeTarget::release_pending() noexcept {
    if (pending_ != nullptr) {
        node_pool().release(std::exchange(pending_, nullptr));
    }
}

} // namespace fiber::http
