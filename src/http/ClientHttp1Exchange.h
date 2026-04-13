#ifndef FIBER_HTTP_CLIENT_HTTP1_EXCHANGE_H
#define FIBER_HTTP_CLIENT_HTTP1_EXCHANGE_H

#include <cstddef>
#include <cstdint>
#include <new>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../common/mem/IoBuf.h"
#include "ClientHttp1Types.h"
#include "Http1HeaderParseBuffer.h"
#include "Http1Parser.h"
#include "HttpExchange.h"

namespace fiber::http {

class Http1ClientConnection;

class ClientHttp1Exchange : public common::NonCopyable, public common::NonMovable {
public:
    ClientHttp1Exchange(Http1ClientConnection &conn, mem::BufPool &pool,
                        Http1ClientExchangeOptions options = {}) noexcept;
    ~ClientHttp1Exchange();

    fiber::async::Task<common::IoResult<void>> send_header(const Http1RequestHead &head, bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(BodyChunk chunk) noexcept;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(const std::uint8_t *buf, std::size_t len,
                                                                 bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<void>> send_trailer(const HttpHeaders &trailers) noexcept;

    fiber::async::Task<common::IoResult<const Http1ResponseHead *>> read_header() noexcept;
    fiber::async::Task<common::IoResult<BodyChunk>> read_body(std::size_t max_bytes = 64 * 1024) noexcept;
    fiber::async::Task<common::IoResult<void>> discard_response_body() noexcept;

    [[nodiscard]] const HttpHeaders &response_trailers() const noexcept { return response_trailers_; }
    [[nodiscard]] const Http1ClientExchangeOptions &options() const noexcept { return options_; }
    [[nodiscard]] bool valid() const noexcept { return active_; }
    [[nodiscard]] bool request_complete() const noexcept { return request_state_ == RequestState::RequestDone; }
    [[nodiscard]] bool response_complete() const noexcept { return response_complete_; }
    [[nodiscard]] bool done() const noexcept { return request_complete() && response_complete_; }

private:
    enum class RequestState : std::uint8_t {
        Init,
        SendingBody,
        RequestDone,
        Failed,
    };

    struct ResponseHeaderNode {
        explicit ResponseHeaderNode(mem::BufPool &pool) : head(pool) {}

        static void *operator new(std::size_t size, mem::BufPool &pool) noexcept {
            return pool.alloc(size, alignof(ResponseHeaderNode));
        }
        static void operator delete(void *ptr, mem::BufPool &) noexcept {}
        static void operator delete(void *ptr) noexcept {}

        Http1ResponseHead head;
        mem::IoBufChain owner_bufs;
        ResponseHeaderNode *next = nullptr;
    };

    void clear_response_header_nodes() noexcept;
    void fail_active_exchange() noexcept;
    common::IoResult<void> ensure_body_read_buf_writable(mem::IoBuf &read_buf, std::size_t min_writable) noexcept;
    common::IoResult<void> take_prefix(mem::IoBuf &read_buf, mem::IoBufChain &out, std::size_t len) noexcept;
    common::IoResult<void> stash_pending_buf(mem::IoBuf &read_buf) noexcept;
    fiber::async::Task<common::IoResult<std::size_t>> read_more(mem::IoBuf &read_buf, std::size_t max_bytes,
                                                                bool &read_call_used_io) noexcept;
    fiber::async::Task<common::IoResult<ParseCode>> advance_chunked_body(mem::IoBuf &read_buf, std::size_t max_bytes,
                                                                         bool allow_read,
                                                                         bool &read_call_used_io) noexcept;
    fiber::async::Task<common::IoResult<void>> read_response_trailers(mem::IoBuf &read_buf) noexcept;

    Http1ClientConnection *conn_ = nullptr;
    mem::BufPool *pool_ = nullptr;
    Http1ClientExchangeOptions options_{};
    HttpHeaders response_trailers_;
    mem::IoBuf pending_buf_;
    ResponseHeaderNode *response_headers_head_ = nullptr;
    BodyParser response_body_parser_{};
    bool active_ = false;
    bool response_complete_ = false;
    bool final_response_received_ = false;
    bool keepalive_on_release_ = false;
    bool saw_connection_close_ = false;
    bool saw_connection_keep_alive_ = false;
    bool response_eof_delimited_ = false;
    RequestState request_state_ = RequestState::Init;
    HttpMethod request_method_ = HttpMethod::Unknown;
    HttpBodySpec body_spec_ = HttpBodySpec::None();
    std::size_t content_length_ = 0;
    std::size_t body_sent_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP1_EXCHANGE_H
