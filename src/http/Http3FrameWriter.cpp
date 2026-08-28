#include "http/Http3FrameWriter.h"

#include <expected>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/Http3Protocol.h>
#include "http/Http3QpackEncoderIoBufWriter.h"
#include "quic/QuicTransportCodec.h"

namespace fiber::http {

namespace {

common::IoResult<void> bind_or_migrate_nodes(mem::IoBufChain &chain, mem::IoBufNodePool &target_pool) noexcept {
    if (!chain.bound()) {
        if (!chain.empty()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        chain.bind_node_pool(target_pool);
        return {};
    }
    if (&chain.node_pool() == &target_pool) {
        return {};
    }

    mem::IoBufChain migrated(target_pool);
    if (chain.complete()) {
        migrated.mark_complete();
    }
    while (mem::IoBufNode *node = chain.pop_front_node()) {
        const bool appended = migrated.append_node(node);
        FIBER_ASSERT(appended);
    }
    chain = std::move(migrated);
    return {};
}

} // namespace

common::IoResult<void> http3_finish_headers_frame(Http3QpackEncoderIoBufWriter &writer, mem::IoBufChain &frame,
                                                  bool end_stream) noexcept {
    common::IoErr err = writer.finish(frame);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }

    const std::size_t total_len = frame.readable_bytes();
    std::uint8_t *reserved = writer.prefix_reserved_data();
    if (reserved == nullptr || writer.prefix_reserved_size() != kHttp3FrameHeaderReserve ||
        total_len < kHttp3FrameHeaderReserve) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t payload_len = total_len - kHttp3FrameHeaderReserve;
    if (static_cast<std::uint64_t>(payload_len) > kMaxHttp3FramePayloadLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t frame_header_len = quic::quic_varint_len(static_cast<std::uint64_t>(Http3FrameType::Headers)) +
                                         quic::quic_varint_len(static_cast<std::uint64_t>(payload_len));
    if (frame_header_len > kHttp3FrameHeaderReserve) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t gap = kHttp3FrameHeaderReserve - frame_header_len;
    quic::QuicWriteCursor out(reserved + gap, frame_header_len);
    auto wrote = quic::quic_write_varint(out, static_cast<std::uint64_t>(Http3FrameType::Headers));
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = quic::quic_write_varint(out, payload_len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    if (out.offset() != frame_header_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (gap != 0) {
        frame.consume_and_compact(gap);
    }
    if (end_stream) {
        frame.mark_complete();
    }
    return {};
}

common::IoResult<mem::IoBuf> http3_build_data_frame_header(std::size_t payload_len) noexcept {
    if (static_cast<std::uint64_t>(payload_len) > kMaxHttp3FramePayloadLength) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (payload_len == 0) {
        return mem::IoBuf{};
    }

    const std::uint64_t frame_type = static_cast<std::uint64_t>(Http3FrameType::Data);
    const std::size_t frame_header_len = quic::quic_varint_len(frame_type) + quic::quic_varint_len(payload_len);
    mem::IoBuf frame_header = mem::IoBuf::allocate(frame_header_len);
    if (!frame_header) {
        return std::unexpected(common::IoErr::NoMem);
    }

    quic::QuicWriteCursor out(frame_header.writable_data(), frame_header_len);
    auto wrote = quic::quic_write_varint(out, frame_type);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = quic::quic_write_varint(out, payload_len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    if (out.offset() != frame_header_len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    frame_header.commit(frame_header_len);
    return frame_header;
}

common::IoResult<void> http3_prepare_data_frame(mem::IoBufChain &chunk, mem::IoBufNodePool &target_pool) noexcept {
    const std::size_t payload_len = chunk.readable_bytes();
    auto bound = bind_or_migrate_nodes(chunk, target_pool);
    if (!bound) {
        return bound;
    }
    auto frame_header = http3_build_data_frame_header(payload_len);
    if (!frame_header) {
        return std::unexpected(frame_header.error());
    }
    if (!*frame_header) {
        return {};
    }
    if (!chunk.prepend(std::move(*frame_header))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

} // namespace fiber::http
