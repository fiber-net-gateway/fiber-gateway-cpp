#ifndef FIBER_HTTP_HTTP3_QPACK_ENCODER_IOBUF_WRITER_H
#define FIBER_HTTP_HTTP3_QPACK_ENCODER_IOBUF_WRITER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http3QpackEncoder.h>

namespace fiber::http {

class Http3QpackEncoderIoBufWriter : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http3QpackEncoderIoBufWriter(mem::IoBufNodePool &node_pool, std::size_t chunk_size = 512) noexcept;
    Http3QpackEncoderIoBufWriter(mem::IoBufNodePool &node_pool, Http3QpackEncoder::Options options,
                                 std::size_t chunk_size = 512) noexcept;
    Http3QpackEncoderIoBufWriter(mem::IoBufNodePool &node_pool, Http3QpackEncoder::Options options,
                                 std::size_t chunk_size, std::size_t prefix_reserve) noexcept;

    [[nodiscard]] common::IoErr encode_status(int status_code) noexcept;
    [[nodiscard]] common::IoErr encode_method(HttpMethod method) noexcept;
    [[nodiscard]] common::IoErr encode_scheme(std::string_view scheme) noexcept;
    [[nodiscard]] common::IoErr encode_authority(std::string_view authority) noexcept;
    [[nodiscard]] common::IoErr encode_path(std::string_view path) noexcept;
    [[nodiscard]] common::IoErr encode_field(std::string_view name, std::uint64_t name_hash,
                                             std::string_view value) noexcept;
    [[nodiscard]] common::IoErr finish(mem::IoBufChain &out) noexcept;
    [[nodiscard]] std::uint8_t *prefix_reserved_data() noexcept { return prefix_reserved_; }
    [[nodiscard]] std::size_t prefix_reserved_size() const noexcept { return prefix_reserve_; }
    void abort() noexcept;

private:
    static const Http3QpackEncoder::OutputOps kOutputOps;

    static common::IoErr acquire_output(void *ctx, std::size_t min_bytes, std::uint8_t *&dst,
                                        std::size_t &len) noexcept;
    static void commit_output(void *ctx, std::size_t written) noexcept;

    mem::IoBufChain block_;
    Http3QpackEncoder encoder_;
    mem::IoBuf *tail_ = nullptr;
    std::uint8_t *prefix_reserved_ = nullptr;
    std::size_t chunk_size_ = 0;
    std::size_t prefix_reserve_ = 0;
    bool finished_ = false;
    bool prefix_reserved_done_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_QPACK_ENCODER_IOBUF_WRITER_H
