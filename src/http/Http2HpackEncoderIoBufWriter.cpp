#include "Http2HpackEncoderIoBufWriter.h"

#include <algorithm>

#include "../common/Assert.h"

namespace fiber::http {

const Http2HpackEncoder::OutputOps Http2HpackEncoderIoBufWriter::kOutputOps{
        &Http2HpackEncoderIoBufWriter::acquire_output,
        &Http2HpackEncoderIoBufWriter::commit_output,
};

Http2HpackEncoderIoBufWriter::Http2HpackEncoderIoBufWriter(Http2HpackEncoder &encoder, mem::IoBufNodePool &node_pool,
                                                           std::size_t chunk_size) noexcept :
    encoder_(encoder), block_(node_pool), chunk_size_(chunk_size) {
    FIBER_ASSERT(chunk_size_ != 0);
}

Http2HpackEncoderIoBufWriter::~Http2HpackEncoderIoBufWriter() {
    if (begun_) {
        abort();
    }
}

common::IoErr Http2HpackEncoderIoBufWriter::begin() noexcept {
    if (begun_) {
        return common::IoErr::Invalid;
    }
    block_.clear();
    tail_ = nullptr;
    common::IoErr err = encoder_.begin_block(this, &kOutputOps);
    if (err != common::IoErr::None) {
        block_.clear();
        tail_ = nullptr;
        return err;
    }
    begun_ = true;
    return common::IoErr::None;
}

common::IoErr Http2HpackEncoderIoBufWriter::encode_status(int status_code) noexcept {
    if (!begun_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_status(status_code);
}

common::IoErr Http2HpackEncoderIoBufWriter::encode_field(std::string_view name, std::uint64_t name_hash,
                                                         std::string_view value) noexcept {
    if (!begun_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_field(name, name_hash, value);
}

common::IoErr Http2HpackEncoderIoBufWriter::finish(mem::IoBufChain &out) noexcept {
    if (!begun_) {
        return common::IoErr::Invalid;
    }
    common::IoErr err = encoder_.finish_block();
    if (err != common::IoErr::None) {
        abort();
        return err;
    }
    out = std::move(block_);
    tail_ = nullptr;
    begun_ = false;
    return common::IoErr::None;
}

void Http2HpackEncoderIoBufWriter::abort() noexcept {
    encoder_.cancel_block();
    block_.clear();
    tail_ = nullptr;
    begun_ = false;
}

common::IoErr Http2HpackEncoderIoBufWriter::acquire_output(void *ctx, std::size_t min_bytes, std::uint8_t *&dst,
                                                           std::size_t &len) noexcept {
    auto *self = static_cast<Http2HpackEncoderIoBufWriter *>(ctx);
    FIBER_ASSERT(self != nullptr);
    if (self->tail_ != nullptr && self->tail_->writable() >= min_bytes) {
        dst = self->tail_->writable_data();
        len = self->tail_->writable();
        return common::IoErr::None;
    }

    const std::size_t cap = std::max(self->chunk_size_, min_bytes);
    mem::IoBuf buf = mem::IoBuf::allocate(cap);
    if (!buf.valid()) {
        return common::IoErr::NoMem;
    }
    if (!self->block_.append(std::move(buf))) {
        return common::IoErr::NoMem;
    }
    self->tail_ = self->block_.first_writable();
    if (self->tail_ == nullptr) {
        return common::IoErr::NoMem;
    }
    dst = self->tail_->writable_data();
    len = self->tail_->writable();
    return common::IoErr::None;
}

void Http2HpackEncoderIoBufWriter::commit_output(void *ctx, std::size_t written) noexcept {
    auto *self = static_cast<Http2HpackEncoderIoBufWriter *>(ctx);
    FIBER_ASSERT(self != nullptr);
    self->block_.commit(written);
    self->tail_ = self->block_.first_writable();
}

} // namespace fiber::http
