#ifndef FIBER_HTTP_CLIENT_HTTP3_REQUEST_H
#define FIBER_HTTP_CLIENT_HTTP3_REQUEST_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/ClientHttp3Types.h>
#include <fiber/http/Http3Codec.h>
#include <fiber/http/Http3Connection.h>
#include <fiber/http/Http3QpackDecoder.h>
#include <fiber/quic/QuicConnection.h>

namespace fiber::http {

class HttpHeaders;

class ClientHttp3Request : public common::NonCopyable, public common::NonMovable {
public:
    ~ClientHttp3Request();

    [[nodiscard]] static quic::QuicStream::Lease create(Http3Connection &conn, mem::BufPool &pool) noexcept;
    [[nodiscard]] static ClientHttp3Request *from_stream(quic::QuicStream &stream) noexcept;
    [[nodiscard]] static const ClientHttp3Request *from_stream(const quic::QuicStream &stream) noexcept;

    [[nodiscard]] common::IoResult<void> register_attached() noexcept;

    async::Task<common::IoResult<void>> send_request_header(const ClientRequestHead &head, bool end_stream,
                                                            std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<std::size_t>> write_all(mem::IoBufChain chunk,
                                                         std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<std::size_t>> write(mem::IoBufChain &chunk,
                                                     std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<std::size_t>> write(const std::uint8_t *buf, std::size_t len, bool end_stream,
                                                     std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> write_trailer(const HttpHeaders &headers,
                                                      std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<const ClientResponseHead *>> read_header(std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<mem::IoBufChain>> read_body(std::size_t max_bytes,
                                                             std::chrono::milliseconds timeout) noexcept;

    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] quic::QuicStream &stream() noexcept { return stream_; }
    [[nodiscard]] const quic::QuicStream &stream() const noexcept { return stream_; }
    [[nodiscard]] mem::IoBufNodePool &node_pool() noexcept;
    [[nodiscard]] Http3ExtendedConnectSupport extended_connect_support() const noexcept;
    [[nodiscard]] Http3RequestOutcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] common::IoErr terminal_error() const noexcept { return terminal_error_; }

private:
    enum class RecvState : std::uint8_t {
        FrameHeader,
        DataPayload,
        WaitFin,
        Complete,
        Error,
    };

    ClientHttp3Request(Http3Connection &conn, mem::BufPool &pool) noexcept;

    static void destroy_owner(void *owner, quic::QuicStream &stream) noexcept;
    static void on_rejected(void *owner, std::uint64_t goaway_id) noexcept;
    static void on_connection_close(void *owner, Http3ErrorCode error) noexcept;
    static const Http3QpackDecoder::Ops &decoder_ops() noexcept;
    static common::IoErr on_indexed_field(void *owner, Http3QpackDecoder::TableEntryView entry) noexcept;
    static common::IoErr on_indexed_name(void *owner, std::string_view name, std::uint64_t name_hash) noexcept;
    static common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;

    async::Task<common::IoResult<void>> write_frame(mem::IoBufChain &frame, std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> write_data_frame_header(std::size_t payload_len,
                                                                std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> read_more_input(std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> skip_frame_payload(std::uint64_t payload_length,
                                                           std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> parse_header_block(bool trailer, std::uint64_t payload_length,
                                                           std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] Http3ParseStatus parse_frame_header_once() noexcept;
    [[nodiscard]] common::IoErr begin_header_block(bool trailer) noexcept;
    [[nodiscard]] common::IoErr complete_header_block() noexcept;
    [[nodiscard]] common::IoErr commit_field(std::string_view name, std::uint64_t name_hash, std::string_view value,
                                             bool stable) noexcept;
    [[nodiscard]] common::IoErr commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                      std::string_view value, bool stable) noexcept;
    [[nodiscard]] common::IoErr handle_status(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_content_length(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr account_field(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] common::IoErr fail_response(Http3ErrorCode error,
                                              common::IoErr reason = common::IoErr::Invalid) noexcept;
    [[nodiscard]] common::IoResult<void> finish_response() noexcept;
    void finish_observation() noexcept;
    void handle_io_error(common::IoErr error) noexcept;
    void reject_from_goaway(std::uint64_t goaway_id) noexcept;
    void handle_connection_close(Http3ErrorCode error) noexcept;
    [[nodiscard]] std::string_view copy_to_pool(std::string_view value) noexcept;

    quic::QuicConnection::Lease quic_lease_{};
    Http3Connection *conn_ = nullptr;
    quic::QuicStream stream_;
    mem::BufPool *pool_ = nullptr;
    mem::IoBufChain inbound_buf_;
    Http3ClientRequestEntry request_entry_{};
    Http3FrameHeaderParser frame_parser_{};
    Http3FrameHeader current_frame_{};
    Http3QpackDecoder qpack_decoder_{};
    ClientResponseHead *current_head_ = nullptr;
    ClientResponseHead *pending_head_ = nullptr;
    std::string_view pending_name_{};
    std::uint64_t pending_name_hash_ = 0;
    std::uint64_t frame_payload_remaining_ = 0;
    std::size_t current_field_section_size_ = 0;
    std::size_t response_body_received_ = 0;
    std::size_t expected_content_length_ = 0;
    std::size_t request_body_sent_ = 0;
    std::size_t request_content_length_ = 0;
    std::size_t request_data_frame_remaining_ = 0;
    HttpMethod request_method_ = HttpMethod::Unknown;
    Http3ErrorCode response_parse_error_ = Http3ErrorCode::GeneralProtocolError;
    common::IoErr terminal_error_ = common::IoErr::None;
    Http3RequestOutcome outcome_ = Http3RequestOutcome::NotSent;
    RecvState recv_state_ = RecvState::FrameHeader;
    bool pending_name_stable_ = false;
    bool current_block_trailer_ = false;
    bool current_block_has_status_ = false;
    bool saw_regular_header_ = false;
    bool current_content_length_seen_ = false;
    bool response_content_length_seen_ = false;
    bool response_no_body_ = false;
    bool request_content_length_seen_ = false;
    bool request_headers_sent_ = false;
    bool request_finished_ = false;
    bool request_data_frame_active_ = false;
    bool request_data_frame_end_ = false;
    bool final_head_received_ = false;
    bool trailer_received_ = false;
    bool frame_header_in_progress_ = false;
    bool reading_ = false;
    bool writing_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP3_REQUEST_H
