#ifndef FIBER_NET_DETAIL_TLS_STREAM_FD_H
#define FIBER_NET_DETAIL_TLS_STREAM_FD_H

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

#include "../../async/Task.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../common/mem/IoBufChain.h"
#include "../../event/Poller.h"
#include "../../tls/TlsConnection.h"
#include "../../tls/record/TlsRecordReader.h"
#include "../TlsParams.h"
#include "StreamFd.h"

namespace fiber::tls {
struct TlsClientHello;
struct TlsServerConfig;
} // namespace fiber::tls

namespace fiber::net::detail {

// TLS over a stream fd, driven by the in-tree tls engines (09 §5): the
// handshake engines run the FSM, TlsConnection seals/opens everything after.
// The socket loop is this class's alone — fd bytes → engine feed, engine
// take_output → fd — over the private per-connection node pool.
class TlsStreamFd : public common::NonCopyable, public common::NonMovable {
public:
    using HandshakeTask = fiber::async::Task<fiber::common::IoResult<void>>;
    using ReadyCallback = StreamFd::ReadyCallback;

    TlsStreamFd(fiber::event::EventLoop &loop, int fd);
    ~TlsStreamFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    // The negotiated protocol (stable until close() or destruction).
    [[nodiscard]] std::string_view selected_alpn() const noexcept;
    [[nodiscard]] bool handshake_done() const noexcept;
    [[nodiscard]] bool has_pending_read() const noexcept;
    [[nodiscard]] bool terminal() const noexcept { return stream_fd_.terminal(); }
    [[nodiscard]] bool peer_closed() const noexcept { return stream_fd_.peer_closed(); }
    [[nodiscard]] bool read_ready() const noexcept { return stream_fd_.read_ready(); }
    [[nodiscard]] bool write_ready() const noexcept { return stream_fd_.write_ready(); }
    void close();

    // Loop handover, see StreamFd. Requires no in-flight operation or pending
    // subscriptions; TLS state (engines, connection, pool) travels with the
    // object — the node pool is a loop-independent freelist.
    fiber::common::IoErr detach_for_handover() noexcept;
    fiber::common::IoErr adopt_loop(fiber::event::EventLoop &loop) noexcept;

    // Idle-pool observation, see StreamFd: listen for the stream-state bits
    // without subscribing a direction callback.
    fiber::common::IoErr ensure_state_observation() noexcept { return stream_fd_.ensure_state_observation(); }

    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept;

    [[nodiscard]] HandshakeTask handshake(const TlsClientParam &param,
                                          std::chrono::milliseconds timeout = kDefaultTlsHandshakeTimeout);
    [[nodiscard]] HandshakeTask handshake(const TlsServerParam &param,
                                          std::chrono::milliseconds timeout = kDefaultTlsHandshakeTimeout);
    [[nodiscard]] StreamFd::WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] StreamFd::WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::common::IoErr poll_shutdown(fiber::event::IoEvent &event) noexcept;
    // Chain-based read: appends one node (capped at a record's plaintext) of
    // freshly decrypted bytes to out and returns them; 0 is EOF (close_notify
    // latched). WouldBlock: wait_readable, then call again. A connected-phase
    // read only ever blocks on readability.
    [[nodiscard]] fiber::common::IoResult<std::size_t> try_read(std::size_t size, mem::IoBufChain &out) noexcept;
    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<std::size_t>>
    readv(std::size_t size, mem::IoBufChain &out, std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    // Chain-based write: prepares one record group from the chain (a node
    // holding whole records passes through zero-copy, smaller runs coalesce
    // into a scratch record), seals and flushes it, then consumes the group
    // from the chain. WouldBlock: the sealed remainder is retained — retry
    // with the same chain after wait_writable; any other chain (empty
    // included) reports Busy until the group completes.
    [[nodiscard]] fiber::common::IoResult<std::size_t> try_write(mem::IoBufChain &buf) noexcept;
    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<std::size_t>>
    writev(mem::IoBufChain &buf, std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    // Drops an in-flight write group's chain identity (post-WouldBlock
    // abandon): the sealed records stay until close, and any write on
    // another chain reports Busy while they linger.
    void abandon_pending_write() noexcept;

private:
    enum class Role : std::uint8_t {
        None,
        Client,
        Server,
    };

    // Handshake staging + engines (defined in the .cpp): a coroutine-frame
    // local of handshake_impl, dying with the frame at co_return or unwind.
    // Staged configs borrow the caller's param material under the documented
    // param contract (valid until the handshake co_returns). The engines'
    // chains resolve the current loop's node pool, so the frame — the Task —
    // must be destroyed on the connection's loop.
    struct Handshake;

    common::IoResult<void> start_client(Handshake &staging, const TlsClientParam &param) noexcept;
    common::IoResult<void> start_server(Handshake &staging, const TlsServerParam &param) noexcept;
    // TlsServerConfigSource::select — re-stages the server config per
    // ClientHello through the param's configure callback.
    static const tls::TlsServerConfig *select_server_config(void *ctx,
                                                            const tls::TlsClientHello &client_hello) noexcept;
    [[nodiscard]] HandshakeTask handshake_impl(Role role, const TlsClientParam *client_param,
                                               const TlsServerParam *server_param, std::chrono::milliseconds timeout);
    fiber::common::IoErr handshake_once(Handshake &staging, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr shutdown_once(fiber::event::IoEvent &event) noexcept;
    // Moves connection output — or the live handshake engines' output when
    // staging is passed — into out_pending_ and writes it out. The connected
    // phase passes nullptr (a live staging outranks nothing there).
    fiber::common::IoErr flush_output(Handshake *staging, fiber::event::IoEvent &event) noexcept;
    // Reads the fd to drain and feeds the live engine (handshake phase).
    fiber::common::IoErr feed_engine(Handshake &staging, fiber::event::IoEvent &event) noexcept;
    // Splits complete records off the connected-phase reader and hands each
    // to the connection (open + route happen there; a trailing partial
    // record stays buffered in record_reader_ across feeds). Reader- and
    // record-level violations both latch the connection's terminal — the
    // read path surfaces it.
    fiber::common::IoErr drain_records() noexcept;

    StreamFd stream_fd_;
    tls::TlsConnection *conn_ = nullptr; // the connected phase
    // Connected-phase framing buffer: fd bytes in, complete records out.
    // Filled from the socket and from the engine's take_inbound_leftover at
    // HandshakeDone; the connection itself never sees partial records.
    tls::TlsRecordReader record_reader_{};
    mem::IoBufChain out_pending_{}; // sealed records not yet on the wire
    mem::IoBufChain early_data_{}; // server: decrypted 0-RTT, delivered first
    // Write-side retry state: the chain whose group is sealed in out_pending_
    // (null once abandoned) and its plaintext length, plus the record
    // coalescing scratch — a plain allocation so it is loop-independent.
    mem::IoBufChain *pending_write_chain_ = nullptr;
    size_t pending_write_len_ = 0;
    std::unique_ptr<std::uint8_t[]> write_scratch_{};
    Role role_ = Role::None;
    bool handshake_done_ = false;
    bool shutdown_started_ = false;
    bool busy_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_STREAM_FD_H
