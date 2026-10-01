#include "http/detail/Http2DataSendOps.h"

#include <algorithm>
#include <cstring>

#include <fiber/common/Assert.h>
#include "http/Http2DataFrameEncoder.h"

namespace fiber::http::detail {

common::IoErr Http2DataSendAllOp::on_encode(const Http2OutboundEncodeRequest &req, Http2OutboundEncodeTarget &target,
                                            Http2OutboundEncodeResult &result) noexcept {
    const std::size_t remaining = chunk_.readable_bytes();
    if (remaining == 0) {
        FIBER_ASSERT(end_);

        Http2DataFrameEncoder frame_encoder({
                .stream_id = req.stream_id,
                .max_frame_size = req.max_frame_size,
                .end_stream = true,
        });
        common::IoErr err = frame_encoder.encode(target, chunk_, 0);
        if (err != common::IoErr::None) {
            return err;
        }
        result.flow_controlled_bytes = 0;
        result.operation_final_batch = true;
        result.end_stream = true;
        return common::IoErr::None;
    }

    FIBER_ASSERT(req.payload_budget != 0);
    const std::size_t payload_budget = std::min<std::size_t>(remaining, req.payload_budget);
    const bool end_stream = end_ && payload_budget == remaining;
    Http2DataFrameEncoder frame_encoder({
            .stream_id = req.stream_id,
            .max_frame_size = req.max_frame_size,
            .end_stream = end_stream,
    });
    common::IoErr err = frame_encoder.encode(target, chunk_, payload_budget);
    if (err != common::IoErr::None) {
        return err;
    }

    result.flow_controlled_bytes = static_cast<std::uint32_t>(payload_budget);
    result.operation_final_batch = chunk_.readable_bytes() == 0;
    result.end_stream = end_stream;
    return common::IoErr::None;
}

common::IoErr Http2DataSendSomeOp::on_encode(const Http2OutboundEncodeRequest &req, Http2OutboundEncodeTarget &target,
                                             Http2OutboundEncodeResult &result) noexcept {
    if (total_bytes_ == 0) {
        FIBER_ASSERT(end_);
        mem::IoBufChain empty;
        Http2DataFrameEncoder frame_encoder({
                .stream_id = req.stream_id,
                .max_frame_size = req.max_frame_size,
                .end_stream = true,
        });
        common::IoErr err = frame_encoder.encode(target, empty, 0);
        if (err != common::IoErr::None) {
            return err;
        }
        // The terminal-only batch (empty complete chain) consumed the borrowed
        // chain's completion marker, same as the data branch below: callers
        // (http::pipe_http_body) require a successful terminal write to flip
        // complete() to false or their completion-progress invariant trips.
        if (chunk_ != nullptr) {
            chunk_->clear_complete();
        }
        result.flow_controlled_bytes = 0;
        result.operation_final_batch = true;
        result.end_stream = true;
        return common::IoErr::None;
    }

    FIBER_ASSERT(req.payload_budget != 0);
    const std::size_t payload_bytes = std::min(total_bytes_, static_cast<std::size_t>(req.payload_budget));
    const bool end_stream = end_ && payload_bytes == total_bytes_;
    mem::IoBufChain staged;
    mem::IoBufChain *payload = chunk_;

    if (chunk_ == nullptr) {
        mem::IoBuf owned = mem::IoBuf::allocate(payload_bytes);
        if (!owned) {
            return common::IoErr::NoMem;
        }
        std::memcpy(owned.writable_data(), buf_, payload_bytes);
        owned.commit(payload_bytes);
        if (!staged.append(std::move(owned))) {
            return common::IoErr::NoMem;
        }
        if (end_stream) {
            staged.mark_complete();
        }
        payload = &staged;
    }

    Http2DataFrameEncoder frame_encoder({
            .stream_id = req.stream_id,
            .max_frame_size = req.max_frame_size,
            .end_stream = end_stream,
    });
    common::IoErr err = frame_encoder.encode(target, *payload, payload_bytes);
    if (err != common::IoErr::None) {
        return err;
    }
    if (chunk_ != nullptr && end_stream) {
        chunk_->clear_complete();
    }

    result.flow_controlled_bytes = static_cast<std::uint32_t>(payload_bytes);
    result.operation_final_batch = true;
    result.end_stream = end_stream;
    return common::IoErr::None;
}

} // namespace fiber::http::detail
