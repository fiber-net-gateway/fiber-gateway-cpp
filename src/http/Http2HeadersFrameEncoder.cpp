#include "Http2HeadersFrameEncoder.h"

#include <algorithm>
#include <cstring>

#include "../common/Assert.h"
#include "Http2OutboundScheduler.h"
#include "Http2Protocol.h"

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

Http2HeadersFrameEncoder::Http2HeadersFrameEncoder(Http2HpackEncoder &encoder, Options options) noexcept :
    encoder_(encoder), options_(options) {}

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
    current_buf_storage_ = {};
    current_frame_header_ = nullptr;

    common::IoErr err = append_payload_buf(first_frame ? first_frame_buf_payload_cap() : next_buf_payload_cap(), true);
    if (err != common::IoErr::None) {
        return err;
    }

    if (first_frame && options_.pad_length != 0) {
        *current_buf_storage_.writable_data() = options_.pad_length;
        commit_to_output(1);
    }
    if (first_frame && options_.has_priority) {
        std::uint8_t *priority = current_buf_storage_.writable_data();
        std::uint32_t dependency = options_.stream_dependency & 0x7fffffffU;
        if (options_.exclusive) {
            dependency |= 0x80000000U;
        }
        priority[0] = static_cast<std::uint8_t>((dependency >> 24) & 0xffU);
        priority[1] = static_cast<std::uint8_t>((dependency >> 16) & 0xffU);
        priority[2] = static_cast<std::uint8_t>((dependency >> 8) & 0xffU);
        priority[3] = static_cast<std::uint8_t>(dependency & 0xffU);
        priority[4] = options_.weight;
        commit_to_output(5);
    }

    current_suffix_len_ = first_frame ? options_.pad_length : 0;
    return common::IoErr::None;
}

common::IoErr Http2HeadersFrameEncoder::append_payload_buf(std::uint32_t payload_cap,
                                                           bool reserve_frame_header) noexcept {
    common::IoErr err = flush_current_buf();
    if (err != common::IoErr::None) {
        return err;
    }

    const std::size_t capacity = static_cast<std::size_t>(payload_cap) + (reserve_frame_header ? kFrameHeaderSize : 0U);
    mem::IoBuf buf = mem::IoBuf::allocate(capacity);
    if (!buf.valid()) {
        return common::IoErr::NoMem;
    }
    if (reserve_frame_header) {
        current_frame_header_ = buf.writable_data();
        buf.commit(kFrameHeaderSize);
    }
    current_buf_storage_ = std::move(buf);
    return common::IoErr::None;
}

common::IoErr Http2HeadersFrameEncoder::seal_current_frame(bool end_headers) noexcept {
    if (current_frame_header_ == nullptr) {
        return common::IoErr::Invalid;
    }

    if (current_suffix_len_ != 0) {
        if (!current_buf_storage_) {
            return common::IoErr::Invalid;
        }
        std::memset(current_buf_storage_.writable_data(), 0, current_suffix_len_);
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
    common::IoErr err = flush_current_buf();
    if (err != common::IoErr::None) {
        return err;
    }
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

std::size_t Http2HeadersFrameEncoder::current_hpack_writable() const noexcept {
    if (!current_buf_storage_) {
        return 0;
    }
    const std::size_t frame_remaining = current_frame_hpack_remaining();
    return std::min<std::size_t>(frame_remaining, current_buf_storage_.writable());
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

void Http2HeadersFrameEncoder::reset_state() noexcept {
    target_ = nullptr;
    current_buf_storage_ = {};
    current_frame_header_ = nullptr;
    current_frame_payload_limit_ = 0;
    current_payload_written_ = 0;
    current_suffix_len_ = 0;
    current_first_frame_ = false;
    begun_ = false;
}

common::IoErr Http2HeadersFrameEncoder::flush_current_buf() noexcept {
    if (!current_buf_storage_ || current_buf_storage_.readable() == 0) {
        current_buf_storage_ = {};
        return common::IoErr::None;
    }

    FIBER_ASSERT(target_ != nullptr);
    return target_->append_buffer(std::move(current_buf_storage_));
}

void Http2HeadersFrameEncoder::commit_to_output(std::size_t bytes) noexcept {
    current_buf_storage_.commit(bytes);
    current_payload_written_ += static_cast<std::uint32_t>(bytes);
}

common::IoErr Http2HeadersFrameEncoder::acquire_output(void *ctx, std::size_t min_bytes, std::uint8_t *&dst,
                                                       std::size_t &len) noexcept {
    auto *self = static_cast<Http2HeadersFrameEncoder *>(ctx);
    FIBER_ASSERT(self != nullptr);
    std::size_t writable = self->current_hpack_writable();
    if (writable < min_bytes) {
        const std::size_t frame_remaining = self->current_frame_hpack_remaining();
        if (frame_remaining == 0) {
            common::IoErr err = self->seal_current_frame(false);
            if (err != common::IoErr::None) {
                return err;
            }
            err = self->open_frame(false);
            if (err != common::IoErr::None) {
                return err;
            }
        } else if (writable == 0 || min_bytes <= frame_remaining) {
            const std::uint32_t next_payload_cap =
                    static_cast<std::uint32_t>(std::min<std::size_t>(frame_remaining, self->next_buf_payload_cap()));
            common::IoErr err = self->append_payload_buf(next_payload_cap, false);
            if (err != common::IoErr::None) {
                return err;
            }
        }
        writable = self->current_hpack_writable();
    }
    if (!self->current_buf_storage_ || writable == 0) {
        return common::IoErr::Invalid;
    }

    dst = self->current_buf_storage_.writable_data();
    len = writable;
    return len != 0 ? common::IoErr::None : common::IoErr::Invalid;
}

void Http2HeadersFrameEncoder::commit_output(void *ctx, std::size_t written) noexcept {
    auto *self = static_cast<Http2HeadersFrameEncoder *>(ctx);
    FIBER_ASSERT(self != nullptr);
    FIBER_ASSERT(written <= self->current_hpack_writable());
    self->commit_to_output(written);
}

} // namespace fiber::http
