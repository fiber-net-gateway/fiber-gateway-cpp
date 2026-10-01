#ifndef FIBER_NET_DETAIL_TLS_STREAM_FD_H
#define FIBER_NET_DETAIL_TLS_STREAM_FD_H

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

#include "../../async/Task.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../common/mem/IoBuf.h"
#include "../../common/mem/IoBufChain.h"
#include "../../event/Poller.h"
#include "../../tls/TlsConnection.h"
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
    // Chain-based read: appends up to a record's worth of freshly decrypted
    // plaintext to out (retained views of the wire buffer it was opened in)
    // and returns its length; 0 is EOF (close_notify latched). WouldBlock:
    // wait_readable, then call again. A connected-phase read only ever blocks
    // on readability. `size` also sizes the wire read (feature/tls/12 §2).
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
    // Each handshake() is its own coroutine owning its staging and engine as
    // frame locals, dying with the frame at co_return or unwind. Staged
    // configs borrow the caller's param material under the documented param
    // contract (valid until the handshake co_returns). The engine's chains
    // resolve the current loop's node pool, so the frame — the Task — must be
    // destroyed on the connection's loop.

    // Start checks shared by both roles: Already, BadFd, version window.
    fiber::common::IoErr check_handshake_start(int min_version, int max_version) const noexcept;
    // TlsServerConfigSource::select — re-stages the server config per
    // ClientHello through the param's configure callback (ctx: the server
    // handshake's frame-local selection, defined in the .cpp).
    static const tls::TlsServerConfig *select_server_config(void *ctx,
                                                            const tls::TlsClientHello &client_hello) noexcept;
    // One drive pass over either engine (instantiated in the .cpp): flush its
    // flights, feed it fd bytes, and at HandshakeDone swap it for the
    // connected-phase connection. A failed engine reports Invalid once its
    // alert is on the wire.
    template<class Engine>
    fiber::common::IoErr handshake_step(Engine &engine, fiber::event::IoEvent &event) noexcept;
    // Reads one wire chunk for the handshake engine (EOF: ConnReset).
    fiber::common::IoErr read_handshake_chunk(mem::IoBuf &chunk, fiber::event::IoEvent &event) noexcept;
    // HandshakeDone: builds the connection from the engine's state and adopts
    // the engine's inbound leftover as inbound_.
    fiber::common::IoErr install_connection(tls::TlsConnectionRole role, tls::TlsConnectedState &&state,
                                            mem::IoBufChain &&leftover) noexcept;
    fiber::common::IoErr shutdown_once(fiber::event::IoEvent &event) noexcept;
    // Moves the connection's output (once connected) into out_pending_ and
    // writes it out; the handshake step queues the engine's output itself.
    fiber::common::IoErr flush_output(fiber::event::IoEvent &event) noexcept;
    // One connected-phase wire read, then process_inbound(): inbound_'s
    // incomplete record continues in its own buffer's tailroom while it fits
    // there; otherwise a fresh buffer sized from the caller's `hint` takes
    // the read, with the incomplete record carried to its head. EOF without
    // close_notify reports ConnReset.
    fiber::common::IoErr read_wire(std::size_t hint) noexcept;
    // Frames every complete record in inbound_ and hands them to the
    // connection in batches (open + route happen there), leaving at most one
    // incomplete record behind. Framing and record violations both latch the
    // connection's terminal — the read path surfaces it.
    void process_inbound() noexcept;

    StreamFd stream_fd_;
    // The connected phase: engaged at HandshakeDone (the engine's state moves
    // in), reset by close(). Held by value — no allocation of its own.
    std::optional<tls::TlsConnection> conn_;
    // Connected-phase wire bytes not framed yet: empty, or ONE incomplete
    // record — the tail of the last wire read (a view into that read's
    // buffer) or of the engine's take_inbound_leftover. It never holds a
    // complete record once process_inbound() returns, so the connection only
    // ever sees whole, contiguous records.
    mem::IoBuf inbound_{};
    mem::IoBufChain out_pending_{}; // sealed records not yet on the wire
    mem::IoBufChain early_data_{}; // server: decrypted 0-RTT, delivered first
    // Write-side retry state: the chain whose group is sealed in out_pending_
    // (null once abandoned) and its plaintext length, plus the record
    // coalescing scratch — a plain allocation so it is loop-independent.
    mem::IoBufChain *pending_write_chain_ = nullptr;
    size_t pending_write_len_ = 0;
    std::unique_ptr<std::uint8_t[]> write_scratch_{};
    bool handshake_started_ = false;
    bool handshake_done_ = false;
    bool shutdown_started_ = false;
    bool busy_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_STREAM_FD_H
