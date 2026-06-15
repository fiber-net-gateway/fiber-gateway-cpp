#include "IoBufChain.h"

#include <algorithm>
#include <new>
#include <utility>

#include "../Assert.h"

namespace fiber::mem {

IoBufNodePool::~IoBufNodePool() { clear(); }

IoBufNode *IoBufNodePool::alloc() noexcept {
    if (free_head_ != nullptr) {
        IoBufNode *node = free_head_;
        free_head_ = node->next;
        --cached_count_;
        node->offset = 0;
        node->state = 0;
        node->buf = {};
        node->next = nullptr;
        return node;
    }
    return new (std::nothrow) IoBufNode{};
}

void IoBufNodePool::release(IoBufNode *node) noexcept {
    if (node == nullptr) {
        return;
    }

    node->offset = 0;
    node->state = 0;
    node->buf = {};
    if (cached_count_ < kIoBufNodePoolMaxCached) {
        node->next = free_head_;
        free_head_ = node;
        ++cached_count_;
        return;
    }

    delete node;
}

void IoBufNodePool::clear() noexcept {
    IoBufNode *node = free_head_;
    while (node != nullptr) {
        IoBufNode *next = node->next;
        delete node;
        node = next;
    }
    free_head_ = nullptr;
    cached_count_ = 0;
}

IoBufChain::IoBufChain(IoBufNodePool &node_pool) noexcept : node_pool_(&node_pool) {}

IoBufChain::~IoBufChain() { clear(); }

IoBufChain::IoBufChain(IoBufChain &&other) noexcept :
    head_(other.head_), tail_(other.tail_), size_(other.size_), readable_bytes_(other.readable_bytes_),
    writable_bytes_(other.writable_bytes_), node_pool_(other.node_pool_), complete_(other.complete_) {
    other.head_ = nullptr;
    other.tail_ = nullptr;
    other.size_ = 0;
    other.readable_bytes_ = 0;
    other.writable_bytes_ = 0;
    other.complete_ = false;
}

IoBufChain &IoBufChain::operator=(IoBufChain &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    clear();
    head_ = other.head_;
    tail_ = other.tail_;
    size_ = other.size_;
    readable_bytes_ = other.readable_bytes_;
    writable_bytes_ = other.writable_bytes_;
    node_pool_ = other.node_pool_;
    complete_ = other.complete_;
    other.head_ = nullptr;
    other.tail_ = nullptr;
    other.size_ = 0;
    other.readable_bytes_ = 0;
    other.writable_bytes_ = 0;
    other.complete_ = false;
    return *this;
}

bool IoBufChain::empty() const noexcept { return size_ == 0; }

std::size_t IoBufChain::size() const noexcept { return size_; }

std::size_t IoBufChain::readable_bytes() const noexcept { return readable_bytes_; }

std::size_t IoBufChain::writable_bytes() const noexcept { return writable_bytes_; }

bool IoBufChain::complete() const noexcept { return complete_; }

IoBufNodePool &IoBufChain::node_pool() noexcept {
    FIBER_ASSERT(node_pool_ != nullptr);
    return *node_pool_;
}

const IoBufNodePool &IoBufChain::node_pool() const noexcept {
    FIBER_ASSERT(node_pool_ != nullptr);
    return *node_pool_;
}

bool IoBufChain::bound() const noexcept { return node_pool_ != nullptr; }

bool IoBufChain::same_pool(const IoBufChain &other) const noexcept { return node_pool_ == other.node_pool_; }

void IoBufChain::bind_node_pool(IoBufNodePool &node_pool) noexcept {
    FIBER_ASSERT(empty());
    FIBER_ASSERT(node_pool_ == nullptr || node_pool_ == &node_pool);
    node_pool_ = &node_pool;
}

bool IoBufChain::append(IoBuf &&buf) noexcept {
    if (!buf) {
        return true;
    }

    IoBufNode *node = node_pool().alloc();
    if (!node) {
        return false;
    }
    node->buf = std::move(buf);
    return append_node(node);
}

bool IoBufChain::prepend(IoBuf &&buf) noexcept {
    if (!buf) {
        return true;
    }

    IoBufNode *node = node_pool().alloc();
    if (!node) {
        return false;
    }
    node->buf = std::move(buf);
    return prepend_node(node);
}

bool IoBufChain::append_node(IoBufNode *node) noexcept {
    FIBER_ASSERT(bound());
    if (node == nullptr) {
        return false;
    }

    std::size_t readable = node->buf.readable();
    std::size_t writable = node->buf.writable();
    reset_node_for_chain(*node);

    if (tail_) {
        tail_->next = node;
    } else {
        head_ = node;
    }
    tail_ = node;
    ++size_;
    readable_bytes_ += readable;
    writable_bytes_ += writable;
    return true;
}

bool IoBufChain::prepend_node(IoBufNode *node) noexcept {
    FIBER_ASSERT(bound());
    if (node == nullptr) {
        return false;
    }

    std::size_t readable = node->buf.readable();
    std::size_t writable = node->buf.writable();
    reset_node_for_chain(*node);

    node->next = head_;
    head_ = node;
    if (!tail_) {
        tail_ = node;
    }
    ++size_;
    readable_bytes_ += readable;
    writable_bytes_ += writable;
    return true;
}

bool IoBufChain::retain_prefix(std::size_t bytes, IoBufChain &out) const noexcept {
    FIBER_ASSERT(bytes <= readable_bytes_);
    FIBER_ASSERT(same_pool(out));

    std::size_t remaining = bytes;
    for (IoBufNode *node = head_; node && remaining > 0; node = node->next) {
        std::size_t readable = node->buf.readable();
        if (readable == 0) {
            continue;
        }

        std::size_t take = std::min(readable, remaining);
        IoBuf slice = node->buf.retain_slice(0, take);
        if (!slice) {
            out.clear();
            return false;
        }
        if (!out.append(std::move(slice))) {
            out.clear();
            return false;
        }
        remaining -= take;
    }

    FIBER_ASSERT(remaining == 0);
    if (bytes == readable_bytes_ && complete_) {
        out.mark_complete();
    }
    return true;
}

bool IoBufChain::take_prefix(std::size_t bytes, IoBufChain &dst) noexcept {
    FIBER_ASSERT(this != &dst);
    FIBER_ASSERT(bytes <= readable_bytes_);
    FIBER_ASSERT(same_pool(dst));
    const bool transfers_complete = bytes == readable_bytes_ && complete_;

    if (bytes == 0) {
        if (transfers_complete) {
            dst.mark_complete();
            complete_ = false;
        }
        return true;
    }

    IoBufNode *boundary = nullptr;
    std::size_t boundary_bytes = 0;
    std::size_t remaining = bytes;
    for (IoBufNode *node = head_; node && remaining > 0; node = node->next) {
        std::size_t readable = node->buf.readable();
        if (readable == 0) {
            continue;
        }
        if (remaining < readable) {
            boundary = node;
            boundary_bytes = remaining;
            break;
        }
        remaining -= readable;
    }

    IoBufNode *partial = nullptr;
    if (boundary_bytes > 0) {
        IoBuf slice = boundary->buf.retain_slice(0, boundary_bytes);
        if (!slice) {
            return false;
        }
        partial = node_pool().alloc();
        if (!partial) {
            return false;
        }
        partial->buf = std::move(slice);
    }

    remaining = bytes;
    IoBufNode *prev = nullptr;
    IoBufNode *node = head_;
    while (node && remaining > 0) {
        IoBufNode *next = node->next;
        std::size_t readable = node->buf.readable();
        if (readable == 0) {
            prev = node;
            node = next;
            continue;
        }

        if (remaining < readable) {
            break;
        }

        std::size_t writable = node->buf.writable();
        if (prev) {
            prev->next = next;
        } else {
            head_ = next;
        }
        if (tail_ == node) {
            tail_ = prev;
        }
        node->next = nullptr;

        --size_;
        readable_bytes_ -= readable;
        writable_bytes_ -= writable;

        if (dst.tail_) {
            dst.tail_->next = node;
        } else {
            dst.head_ = node;
        }
        dst.tail_ = node;
        ++dst.size_;
        dst.readable_bytes_ += readable;
        dst.writable_bytes_ += writable;

        remaining -= readable;
        node = next;
    }

    if (boundary_bytes > 0) {
        FIBER_ASSERT(node == boundary);
        boundary->buf.consume(boundary_bytes);
        readable_bytes_ -= boundary_bytes;

        if (dst.tail_) {
            dst.tail_->next = partial;
        } else {
            dst.head_ = partial;
        }
        dst.tail_ = partial;
        ++dst.size_;
        dst.readable_bytes_ += boundary_bytes;
    }

    if (transfers_complete) {
        dst.mark_complete();
        complete_ = false;
    }

    return true;
}

void IoBufChain::clear() noexcept {
    release_nodes(head_);
    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
    readable_bytes_ = 0;
    writable_bytes_ = 0;
    complete_ = false;
}

void IoBufChain::consume(std::size_t bytes) noexcept {
    FIBER_ASSERT(bytes <= readable_bytes_);
    readable_bytes_ -= bytes;
    for (IoBufNode *node = head_; node && bytes > 0; node = node->next) {
        std::size_t readable = node->buf.readable();
        if (bytes < readable) {
            node->buf.consume(bytes);
            return;
        }
        node->buf.consume(readable);
        bytes -= readable;
    }
}

void IoBufChain::drop_empty_front() noexcept {
    while (head_ && head_->buf.readable() == 0) {
        writable_bytes_ -= head_->buf.writable();
        IoBufNode *next = head_->next;
        node_pool().release(head_);
        head_ = next;
        --size_;
    }
    if (!head_) {
        tail_ = nullptr;
    }
}

void IoBufChain::consume_and_compact(std::size_t bytes) noexcept {
    FIBER_ASSERT(bytes <= readable_bytes_);
    readable_bytes_ -= bytes;

    IoBufNode *node = head_;
    while (node && bytes > 0) {
        std::size_t readable = node->buf.readable();
        if (bytes < readable) {
            node->buf.consume(bytes);
            head_ = node;
            return;
        }

        node->buf.consume(readable);
        bytes -= readable;

        IoBufNode *next = node->next;
        writable_bytes_ -= node->buf.writable();
        node_pool().release(node);
        node = next;
        --size_;
    }

    head_ = node;
    if (!head_) {
        tail_ = nullptr;
    }
}

void IoBufChain::commit(std::size_t bytes) noexcept {
    FIBER_ASSERT(bytes <= writable_bytes_);
    writable_bytes_ -= bytes;
    readable_bytes_ += bytes;
    for (IoBufNode *node = head_; node && bytes > 0; node = node->next) {
        std::size_t writable = node->buf.writable();
        if (bytes < writable) {
            node->buf.commit(bytes);
            return;
        }
        node->buf.commit(writable);
        bytes -= writable;
    }
}

void IoBufChain::mark_complete() noexcept { complete_ = true; }

void IoBufChain::clear_complete() noexcept { complete_ = false; }

IoBufNode *IoBufChain::pop_front_node() noexcept {
    FIBER_ASSERT(bound());
    IoBufNode *node = head_;
    if (node == nullptr) {
        return nullptr;
    }

    head_ = node->next;
    if (tail_ == node) {
        tail_ = nullptr;
    }
    node->next = nullptr;
    --size_;
    readable_bytes_ -= node->buf.readable();
    writable_bytes_ -= node->buf.writable();
    if (head_ == nullptr) {
        tail_ = nullptr;
    }
    return node;
}

int IoBufChain::fill_write_iov(struct iovec *iov, int max_iov) const noexcept {
    if (!iov || max_iov <= 0) {
        return 0;
    }

    int count = 0;
    for (IoBufNode *node = head_; node && count < max_iov; node = node->next) {
        std::size_t len = node->buf.readable();
        if (len == 0) {
            continue;
        }
        iov[count].iov_base = const_cast<std::uint8_t *>(node->buf.readable_data());
        iov[count].iov_len = len;
        ++count;
    }
    return count;
}

int IoBufChain::fill_read_iov(struct iovec *iov, int max_iov) const noexcept {
    if (!iov || max_iov <= 0) {
        return 0;
    }

    int count = 0;
    for (IoBufNode *node = head_; node && count < max_iov; node = node->next) {
        std::size_t len = node->buf.writable();
        if (len == 0) {
            continue;
        }
        iov[count].iov_base = node->buf.writable_data();
        iov[count].iov_len = len;
        ++count;
    }
    return count;
}

IoBuf *IoBufChain::front() noexcept { return head_ ? &head_->buf : nullptr; }

const IoBuf *IoBufChain::front() const noexcept { return head_ ? &head_->buf : nullptr; }

IoBuf *IoBufChain::first_readable() noexcept {
    for (IoBufNode *node = head_; node; node = node->next) {
        if (node->buf.readable() > 0) {
            return &node->buf;
        }
    }
    return nullptr;
}

const IoBuf *IoBufChain::first_readable() const noexcept {
    for (IoBufNode *node = head_; node; node = node->next) {
        if (node->buf.readable() > 0) {
            return &node->buf;
        }
    }
    return nullptr;
}

IoBuf *IoBufChain::first_writable() noexcept {
    for (IoBufNode *node = head_; node; node = node->next) {
        if (node->buf.writable() > 0) {
            return &node->buf;
        }
    }
    return nullptr;
}

const IoBuf *IoBufChain::first_writable() const noexcept {
    for (IoBufNode *node = head_; node; node = node->next) {
        if (node->buf.writable() > 0) {
            return &node->buf;
        }
    }
    return nullptr;
}

void IoBufChain::release_nodes(IoBufNode *node) noexcept {
    while (node) {
        IoBufNode *next = node->next;
        node_pool().release(node);
        node = next;
    }
}

void IoBufChain::reset_node_for_chain(IoBufNode &node) noexcept {
    node.offset = 0;
    node.state = 0;
    node.next = nullptr;
}

} // namespace fiber::mem
