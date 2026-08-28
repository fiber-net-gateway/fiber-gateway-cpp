#include "http/Http3QpackEncoderIoBufWriter.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::http {

const Http3QpackEncoder::OutputOps Http3QpackEncoderIoBufWriter::kOutputOps{
        &Http3QpackEncoderIoBufWriter::acquire_output,
        &Http3QpackEncoderIoBufWriter::commit_output,
};

Http3QpackEncoderIoBufWriter::Http3QpackEncoderIoBufWriter(mem::IoBufNodePool &node_pool,
                                                           std::size_t chunk_size) noexcept :
    Http3QpackEncoderIoBufWriter(node_pool, Http3QpackEncoder::Options{}, chunk_size) {}

Http3QpackEncoderIoBufWriter::Http3QpackEncoderIoBufWriter(mem::IoBufNodePool &node_pool,
                                                           Http3QpackEncoder::Options options,
                                                           std::size_t chunk_size) noexcept :
    Http3QpackEncoderIoBufWriter(node_pool, options, chunk_size, 0) {}

Http3QpackEncoderIoBufWriter::Http3QpackEncoderIoBufWriter(mem::IoBufNodePool &node_pool,
                                                           Http3QpackEncoder::Options options, std::size_t chunk_size,
                                                           std::size_t prefix_reserve) noexcept :
    block_(node_pool), encoder_(this, kOutputOps, options), chunk_size_(chunk_size), prefix_reserve_(prefix_reserve) {
    FIBER_ASSERT(chunk_size_ != 0);
}

common::IoErr Http3QpackEncoderIoBufWriter::encode_status(int status_code) noexcept {
    FIBER_ASSERT(!finished_);
    return encoder_.encode_status(status_code);
}

common::IoErr Http3QpackEncoderIoBufWriter::encode_method(HttpMethod method) noexcept {
    FIBER_ASSERT(!finished_);
    return encoder_.encode_method(method);
}

common::IoErr Http3QpackEncoderIoBufWriter::encode_scheme(std::string_view scheme) noexcept {
    FIBER_ASSERT(!finished_);
    return encoder_.encode_scheme(scheme);
}

common::IoErr Http3QpackEncoderIoBufWriter::encode_authority(std::string_view authority) noexcept {
    FIBER_ASSERT(!finished_);
    return encoder_.encode_authority(authority);
}

common::IoErr Http3QpackEncoderIoBufWriter::encode_path(std::string_view path) noexcept {
    FIBER_ASSERT(!finished_);
    return encoder_.encode_path(path);
}

common::IoErr Http3QpackEncoderIoBufWriter::encode_field(std::string_view name, std::uint64_t name_hash,
                                                         std::string_view value) noexcept {
    FIBER_ASSERT(!finished_);
    return encoder_.encode_field(name, name_hash, value);
}

common::IoErr Http3QpackEncoderIoBufWriter::finish(mem::IoBufChain &out) noexcept {
    FIBER_ASSERT(!finished_);
    common::IoErr err = encoder_.finish();
    if (err != common::IoErr::None) {
        abort();
        return err;
    }
    out = std::move(block_);
    tail_ = nullptr;
    finished_ = true;
    return common::IoErr::None;
}

void Http3QpackEncoderIoBufWriter::abort() noexcept {
    block_.clear();
    tail_ = nullptr;
    prefix_reserved_ = nullptr;
    finished_ = true;
}

common::IoErr Http3QpackEncoderIoBufWriter::acquire_output(void *ctx, std::size_t min_bytes, std::uint8_t *&dst,
                                                           std::size_t &len) noexcept {
    auto *self = static_cast<Http3QpackEncoderIoBufWriter *>(ctx);
    FIBER_ASSERT(self != nullptr);
    if (self->tail_ != nullptr && self->tail_->writable() >= min_bytes) {
        dst = self->tail_->writable_data();
        len = self->tail_->writable();
        return common::IoErr::None;
    }

    const std::size_t reserve = self->prefix_reserved_done_ ? 0 : self->prefix_reserve_;
    if (reserve > std::numeric_limits<std::size_t>::max() - min_bytes) {
        return common::IoErr::NoMem;
    }
    const std::size_t cap = std::max(self->chunk_size_, min_bytes + reserve);
    mem::IoBuf buf = mem::IoBuf::allocate(cap);
    if (!buf.valid()) {
        return common::IoErr::NoMem;
    }
    if (!self->block_.append(std::move(buf))) {
        return common::IoErr::NoMem;
    }
    self->tail_ = self->block_.back();
    if (self->tail_ == nullptr) {
        return common::IoErr::NoMem;
    }
    if (!self->prefix_reserved_done_) {
        if (self->prefix_reserve_ != 0) {
            if (self->tail_->writable() < self->prefix_reserve_) {
                return common::IoErr::NoMem;
            }
            self->prefix_reserved_ = self->tail_->writable_data();
            self->block_.commit_back(self->prefix_reserve_);
            if (self->tail_->writable() < min_bytes) {
                return common::IoErr::NoMem;
            }
        }
        self->prefix_reserved_done_ = true;
    }
    dst = self->tail_->writable_data();
    len = self->tail_->writable();
    return common::IoErr::None;
}

void Http3QpackEncoderIoBufWriter::commit_output(void *ctx, std::size_t written) noexcept {
    auto *self = static_cast<Http3QpackEncoderIoBufWriter *>(ctx);
    FIBER_ASSERT(self != nullptr);
    FIBER_ASSERT(self->tail_ != nullptr);
    FIBER_ASSERT(written <= self->tail_->writable());
    self->block_.commit_back(written);
}

} // namespace fiber::http
