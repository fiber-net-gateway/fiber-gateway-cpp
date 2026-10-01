#include "http/Http2HeadersFrameEncoder.h"

#include <algorithm>
#include <cstring>

#include <fiber/common/Assert.h>
#include <fiber/http/Http2Outbound.h>
#include <fiber/http/Http2Protocol.h>

namespace fiber::http {

namespace {

constexpr std::size_t kFrameHeaderSize = 9;
constexpr std::uint8_t kFlagEndStream = 0x1;
constexpr std::uint8_t kFlagEndHeaders = 0x4;
constexpr std::uint8_t kFlagPadded = 0x8;
constexpr std::uint8_t kFlagPriority = 0x20;

} // namespace

const Http2HpackEncoder::OutputOps Http2HeadersFrameEncoder::kOutputOps{
        &Http2HeadersFrameEncoder::acquire_output,
        &Http2HeadersFrameEncoder::commit_output,
};

Http2HeadersFrameEncoder::Http2HeadersFrameEncoder(Options options) noexcept :
    encoder_(options.hpack), options_(options) {}

Http2HeadersFrameEncoder::~Http2HeadersFrameEncoder() {
    if (begun_) {
        abort();
    }
}

common::IoErr Http2HeadersFrameEncoder::begin(Http2OutboundEncodeTarget &target) noexcept {
    if (begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    target_ = &target;
    common::IoErr err = validate_options();
    if (err != common::IoErr::None) {
        target_ = nullptr;
        return err;
    }
    err = open_frame(true);
    if (err != common::IoErr::None) {
        reset_state();
        return err;
    }
    err = encoder_.begin_block(this, &kOutputOps);
    if (err != common::IoErr::None) {
        reset_state();
        return err;
    }
    begun_ = true;
    return common::IoErr::None;
}

common::IoErr Http2HeadersFrameEncoder::encode_status(int status_code) noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_status(status_code);
}

common::IoErr Http2HeadersFrameEncoder::encode_method(HttpMethod method) noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_method(method);
}

common::IoErr Http2HeadersFrameEncoder::encode_scheme(std::string_view scheme) noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_scheme(scheme);
}

common::IoErr Http2HeadersFrameEncoder::encode_authority(std::string_view authority) noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_authority(authority);
}

common::IoErr Http2HeadersFrameEncoder::encode_path(std::string_view path) noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_path(path);
}

common::IoErr Http2HeadersFrameEncoder::encode_protocol(std::string_view protocol) noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_protocol(protocol);
}

common::IoErr Http2HeadersFrameEncoder::encode_field(std::string_view name, std::uint64_t name_hash,
                                                     std::string_view value) noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }
    return encoder_.encode_field(name, name_hash, value);
}

common::IoErr Http2HeadersFrameEncoder::finish() noexcept {
    if (!begun_ || finished_) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = encoder_.finish_block();
    if (err != common::IoErr::None) {
        abort();
        return err;
    }

    err = seal_current_frame(true);
    if (err != common::IoErr::None) {
        abort();
        return err;
    }

    reset_state();
    finished_ = true;
    return common::IoErr::None;
}

void Http2HeadersFrameEncoder::abort() noexcept {
    encoder_.cancel_block();
    reset_state();
}

common::IoErr Http2HeadersFrameEncoder::open_frame(bool first_frame) noexcept {
    current_first_frame_ = first_frame;
    current_frame_payload_limit_ = options_.max_frame_size;
    current_payload_written_ = 0;
    current_suffix_len_ = 0;
    current_frame_header_ = nullptr;

    const std::size_t prefix =
            first_frame ? (options_.pad_length != 0 ? 1U : 0U) + (options_.has_priority ? 5U : 0U) : 0U;
    std::uint8_t *dst = nullptr;
    std::size_t room = 0;
    common::IoErr err =
            target_->acquire(kFrameHeaderSize + prefix, kFrameHeaderSize + fresh_buf_payload_cap(), dst, room);
    if (err != common::IoErr::None) {
        return err;
    }
    current_frame_header_ = dst;
    std::uint8_t *out = dst + kFrameHeaderSize;
    if (first_frame && options_.pad_length != 0) {
        *out++ = options_.pad_length;
    }
    if (first_frame && options_.has_priority) {
        std::uint32_t dependency = options_.stream_dependency & 0x7fffffffU;
        if (options_.exclusive) {
            dependency |= 0x80000000U;
        }
        out[0] = static_cast<std::uint8_t>((dependency >> 24) & 0xffU);
        out[1] = static_cast<std::uint8_t>((dependency >> 16) & 0xffU);
        out[2] = static_cast<std::uint8_t>((dependency >> 8) & 0xffU);
        out[3] = static_cast<std::uint8_t>(dependency & 0xffU);
        out[4] = options_.weight;
    }
    target_->commit(kFrameHeaderSize);
    commit_to_output(prefix);

    current_suffix_len_ = first_frame ? options_.pad_length : 0;
    return common::IoErr::None;
}

common::IoErr Http2HeadersFrameEncoder::seal_current_frame(bool end_headers) noexcept {
    if (current_frame_header_ == nullptr) {
        return common::IoErr::Invalid;
    }

    if (current_suffix_len_ != 0) {
        std::uint8_t *padding = nullptr;
        std::size_t room = 0;
        common::IoErr err = target_->acquire(current_suffix_len_, current_suffix_len_, padding, room);
        if (err != common::IoErr::None) {
            return err;
        }
        std::memset(padding, 0, current_suffix_len_);
        commit_to_output(current_suffix_len_);
    }

    std::uint8_t flags = 0;
    Http2FrameType type = current_first_frame_ ? Http2FrameType::Headers : Http2FrameType::Continuation;
    if (end_headers) {
        flags |= kFlagEndHeaders;
    }
    if (current_first_frame_) {
        if (options_.end_stream) {
            flags |= kFlagEndStream;
        }
        if (options_.pad_length != 0) {
            flags |= kFlagPadded;
        }
        if (options_.has_priority) {
            flags |= kFlagPriority;
        }
    }

    encode_http2_frame_header(current_frame_header_, current_payload_written_, type, flags, options_.stream_id);
    current_frame_header_ = nullptr;
    current_frame_payload_limit_ = 0;
    current_payload_written_ = 0;
    current_suffix_len_ = 0;
    current_first_frame_ = false;
    return common::IoErr::None;
}

common::IoErr Http2HeadersFrameEncoder::validate_options() const noexcept {
    if (options_.stream_id == 0 || options_.max_frame_size == 0) {
        return common::IoErr::Invalid;
    }
    const std::uint32_t first_cap = first_frame_buf_payload_cap();
    const std::uint32_t reserved =
            (options_.pad_length != 0 ? 1U : 0U) + (options_.has_priority ? 5U : 0U) + options_.pad_length;
    if (reserved > options_.max_frame_size || reserved > first_cap) {
        return common::IoErr::Invalid;
    }
    return common::IoErr::None;
}

std::size_t Http2HeadersFrameEncoder::current_frame_hpack_remaining() const noexcept {
    if (current_frame_payload_limit_ < current_payload_written_ + current_suffix_len_) {
        return 0;
    }
    return current_frame_payload_limit_ - current_payload_written_ - current_suffix_len_;
}

std::uint32_t Http2HeadersFrameEncoder::first_frame_buf_payload_cap() const noexcept {
    if (options_.first_frame_payload_cap == 0) {
        return options_.max_frame_size;
    }
    return std::min<std::uint32_t>(options_.max_frame_size, options_.first_frame_payload_cap);
}

std::uint32_t Http2HeadersFrameEncoder::next_buf_payload_cap() const noexcept { return options_.max_frame_size; }

// Payload capacity for a fresh buffer when the target's tail has no room. The
// first frame starts small and grows with what it already holds, so a short
// block that overflows a reused tail does not take a whole frame's worth.
std::size_t Http2HeadersFrameEncoder::fresh_buf_payload_cap() const noexcept {
    const std::size_t cap = current_first_frame_
                                    ? std::max<std::size_t>(first_frame_buf_payload_cap(), current_payload_written_)
                                    : next_buf_payload_cap();
    return std::min(cap, current_frame_hpack_remaining());
}

void Http2HeadersFrameEncoder::reset_state() noexcept {
    target_ = nullptr;
    current_frame_header_ = nullptr;
    current_frame_payload_limit_ = 0;
    current_payload_written_ = 0;
    current_suffix_len_ = 0;
    current_first_frame_ = false;
    begun_ = false;
}

void Http2HeadersFrameEncoder::commit_to_output(std::size_t bytes) noexcept {
    target_->commit(bytes);
    current_payload_written_ += static_cast<std::uint32_t>(bytes);
}

common::IoErr Http2HeadersFrameEncoder::acquire_output(void *ctx, std::size_t min_bytes, std::uint8_t *&dst,
                                                       std::size_t &len) noexcept {
    auto *self = static_cast<Http2HeadersFrameEncoder *>(ctx);
    FIBER_ASSERT(self != nullptr);
    if (self->current_frame_hpack_remaining() == 0) {
        common::IoErr err = self->seal_current_frame(false);
        if (err != common::IoErr::None) {
            return err;
        }
        err = self->open_frame(false);
        if (err != common::IoErr::None) {
            return err;
        }
    }

    // A request the frame cannot hold whole gets what the frame has left;
    // the HPACK encoder then writes it in pieces.
    const std::size_t frame_remaining = self->current_frame_hpack_remaining();
    std::size_t room = 0;
    common::IoErr err =
            self->target_->acquire(std::min(min_bytes, frame_remaining), self->fresh_buf_payload_cap(), dst, room);
    if (err != common::IoErr::None) {
        return err;
    }
    len = std::min(room, frame_remaining);
    return common::IoErr::None;
}

void Http2HeadersFrameEncoder::commit_output(void *ctx, std::size_t written) noexcept {
    auto *self = static_cast<Http2HeadersFrameEncoder *>(ctx);
    FIBER_ASSERT(self != nullptr);
    FIBER_ASSERT(written <= self->current_frame_hpack_remaining());
    self->commit_to_output(written);
}

} // namespace fiber::http
