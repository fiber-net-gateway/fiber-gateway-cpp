#include "Http2DataFrameEncoder.h"

#include <algorithm>
#include <array>

#include "Http2OutboundScheduler.h"
#include "Http2Protocol.h"

namespace fiber::http {

namespace {

constexpr std::uint8_t kFlagEndStream = 0x1;

} // namespace

common::IoErr Http2DataFrameEncoder::encode(Http2OutboundEncodeTarget &target, mem::IoBufChain &payload,
                                            std::size_t payload_bytes) noexcept {
    common::IoErr err = validate_options();
    if (err != common::IoErr::None) {
        return err;
    }
    if (payload_bytes > payload.readable_bytes()) {
        return common::IoErr::Invalid;
    }

    if (payload_bytes == 0) {
        if (!options_.end_stream) {
            return common::IoErr::None;
        }
        return append_frame(target, payload, 0, true);
    }

    std::size_t remaining = payload_bytes;
    while (remaining != 0) {
        const std::uint32_t frame_payload_bytes =
            static_cast<std::uint32_t>(std::min<std::size_t>(remaining, options_.max_frame_size));
        const bool end_stream = options_.end_stream && remaining == frame_payload_bytes;
        err = append_frame(target, payload, frame_payload_bytes, end_stream);
        if (err != common::IoErr::None) {
            return err;
        }
        remaining -= frame_payload_bytes;
    }

    return common::IoErr::None;
}

common::IoErr Http2DataFrameEncoder::validate_options() const noexcept {
    if (options_.stream_id == 0 || options_.max_frame_size == 0) {
        return common::IoErr::Invalid;
    }
    return common::IoErr::None;
}

common::IoErr Http2DataFrameEncoder::append_frame(Http2OutboundEncodeTarget &target, mem::IoBufChain &payload,
                                                  std::uint32_t payload_bytes, bool end_stream) noexcept {
    std::array<std::uint8_t, 9> header{};
    encode_http2_frame_header(header.data(), payload_bytes, Http2FrameType::Data,
                              end_stream ? kFlagEndStream : 0, options_.stream_id);
    common::IoErr err = target.append_copy(header.data(), header.size());
    if (err != common::IoErr::None) {
        return err;
    }
    if (payload_bytes == 0) {
        return common::IoErr::None;
    }

    mem::IoBufChain frame_payload;
    if (!payload.take_prefix(payload_bytes, frame_payload)) {
        return common::IoErr::NoMem;
    }
    return target.append_chain(std::move(frame_payload));
}

} // namespace fiber::http
