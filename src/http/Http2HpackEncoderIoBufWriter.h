#ifndef FIBER_HTTP_HTTP2_HPACK_ENCODER_IOBUF_WRITER_H
#define FIBER_HTTP_HTTP2_HPACK_ENCODER_IOBUF_WRITER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "Http2HpackEncoder.h"

namespace fiber::http {

class Http2HpackEncoderIoBufWriter : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http2HpackEncoderIoBufWriter(Http2HpackEncoder &encoder, std::size_t chunk_size = 512) noexcept;
    ~Http2HpackEncoderIoBufWriter();

    [[nodiscard]] common::IoErr begin() noexcept;
    [[nodiscard]] common::IoErr encode_status(int status_code) noexcept;
    [[nodiscard]] common::IoErr encode_field(std::string_view name, std::uint64_t name_hash,
                                             std::string_view value) noexcept;
    [[nodiscard]] common::IoErr finish(mem::IoBufChain &out) noexcept;
    void abort() noexcept;

private:
    static const Http2HpackEncoder::OutputOps kOutputOps;

    static common::IoErr acquire_output(void *ctx, std::size_t min_bytes, std::uint8_t *&dst,
                                        std::size_t &len) noexcept;
    static void commit_output(void *ctx, std::size_t written) noexcept;

    Http2HpackEncoder &encoder_;
    mem::IoBufChain block_;
    mem::IoBuf *tail_ = nullptr;
    std::size_t chunk_size_ = 0;
    bool begun_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_ENCODER_IOBUF_WRITER_H
