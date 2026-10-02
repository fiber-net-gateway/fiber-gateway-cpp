#ifndef FIBER_NET_DETAIL_TLS_STREAM_FD_H
#define FIBER_NET_DETAIL_TLS_STREAM_FD_H

#include <chrono>
#include <coroutine>
#include <cstddef>
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
//
// Write side (feature/tls/14): sealed is accepted. A write that seals a batch
// reports it at once; whatever the socket did not take stays in out_pending_
// and drains on its own through this object's write subscription on the
// StreamFd. Until it is out the write direction is not ready: try_write
// reports WouldBlock, and the TLS-level write subscriber (set_write_callback
// or wait_writable) is notified only once the drain is done.
class TlsStreamFd : public common::NonCopyable, public common::NonMovable {
public:
    using HandshakeTask = fiber::async::Task<fiber::common::IoResult<void>>;
    using ReadyCallback = StreamFd::ReadyCallback;

    // Waits for write_ready(), local to the stream's loop. It is installed as
    // the TLS-level write subscription, so it shares that slot with
    // set_write_callback, and like RWFd's awaiter it unsubscribes when
    // destroyed while suspended (how timeout_for abandons it).
    class WaitWritableAwaiter : public common::NonCopyable, public common::NonMovable {
    public:
        WaitWritableAwaiter(TlsStreamFd &stream, std::chrono::milliseconds timeout) noexcept;
        ~WaitWritableAwaiter();

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        fiber::common::IoResult<void> await_resume() noexcept;

    private:
        static void on_ready(void *ctx, fiber::common::IoErr err) noexcept;
        static void on_timeout(WaitWritableAwaiter *awaiter) noexcept;
        void cancel_timer() noexcept;

        TlsStreamFd *stream_;
        std::chrono::milliseconds timeout_;
        fiber::event::EventLoop *loop_ = nullptr;
        fiber::event::EventLoop::TimerEntry timer_entry_{};
        std::coroutine_handle<> coro_{};
        fiber::common::IoErr err_ = fiber::common::IoErr::None;
        bool waiting_ = false;
    };

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
    // Buffered plaintext (or a latched read result, see has_pending_read)
    // reads like a Ready fd: its wire bytes are already consumed, so no
    // socket edge will announce it.
    [[nodiscard]] bool read_ready() const noexcept { return has_pending_read() || stream_fd_.read_ready(); }
    // Sealed output still draining makes the write direction not ready; a
    // latched write error makes it ready (the next write reports it).
    [[nodiscard]] bool write_ready() const noexcept {
        return write_error_ != fiber::common::IoErr::None || (out_pending_.empty() && stream_fd_.write_ready());
    }
    // Accepted (sealed) output the socket has not taken yet.
    [[nodiscard]] bool has_pending_write() const noexcept { return !out_pending_.empty(); }
    void close();

    // Loop handover, see StreamFd. Requires no in-flight operation, pending
    // subscriptions or undrained output (writev users — the handed-over H1
    // pool connections — never leave any); TLS state (engines, connection,
    // pool) travels with the object — the node pool is a loop-independent
    // freelist.
    fiber::common::IoErr detach_for_handover() noexcept;
    fiber::common::IoErr adopt_loop(fiber::event::EventLoop &loop) noexcept;

    // Idle-pool observation, see StreamFd: listen for the stream-state bits
    // without subscribing a direction callback.
    fiber::common::IoErr ensure_state_observation() noexcept { return stream_fd_.ensure_state_observation(); }

    // Subscriptions follow RWFd's contract with read_ready()/write_ready() as
    // the directions' state: a ready caller advances by doing I/O — buffered
    // plaintext is never announced by a socket edge, and the write subscriber
    // is notified only once the sealed output has drained. Write callbacks
    // run on this object's own StreamFd subscription, after its drain step.
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
    // Completes without parking while has_pending_read(), like a wait on a
    // Ready fd (a zero timeout still reports TimedOut, as for any RWFd wait).
    [[nodiscard]] StreamFd::WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::common::IoErr poll_shutdown(fiber::event::IoEvent &event) noexcept;
    // Chain-based read: appends up to `size` bytes of decrypted plaintext to
    // out — whatever is buffered, possibly several records' worth (retained
    // views of the wire buffer it was opened in) — and returns its length; 0
    // is EOF (close_notify latched). The wire is read only while nothing is
    // buffered, and `size` sizes that read (feature/tls/12 §2). WouldBlock:
    // wait_readable, then call again. A connected-phase read only ever blocks
    // on readability.
    [[nodiscard]] fiber::common::IoResult<std::size_t> try_read(std::size_t size, mem::IoBufChain &out) noexcept;
    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<std::size_t>>
    readv(std::size_t size, mem::IoBufChain &out, std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    // Chain-based write: seals a batch of record groups from the chain (a
    // node holding whole records passes through zero-copy, smaller runs
    // coalesce into a scratch record) until ~64 KiB of plaintext, flushes
    // them in one go (feature/tls/13), consumes the batch from the chain and
    // returns its length — even when the socket did not take all of it: the
    // rest drains on its own (feature/tls/14). WouldBlock: an earlier batch is
    // still draining; wait for write readiness, then call again with any
    // chain. A failed flush or NoMem while sealing is connection-fatal and
    // latched: every later write and poll_shutdown report it, and close()
    // sends nothing more.
    [[nodiscard]] fiber::common::IoResult<std::size_t> try_write(mem::IoBufChain &buf) noexcept;
    // One try_write batch per call, returned once it is on the wire: unlike
    // try_write, the coroutine also waits out the drain (an empty chain just
    // waits for it), so a writev user never leaves sealed output behind.
    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<std::size_t>>
    writev(mem::IoBufChain &buf, std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

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
    // wait_readable's veto (ctx: this): buffered plaintext proceeds at once,
    // everything else defers to the stream's own gate.
    static fiber::common::IoResult<bool> on_read_wait_gate(void *ctx, fiber::event::IoEvent direction) noexcept;
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
    // Seals one write batch off the head of `buf` into the connection's
    // output: groups back to back until kWriteBatchBytes of plaintext (a soft
    // cap — one record may overshoot) or the chain's end. `batch_len` reports
    // the plaintext sealed.
    fiber::common::IoErr seal_write_batch(const mem::IoBufChain &buf, std::size_t &batch_len) noexcept;
    // Moves the connection's output (once connected) into out_pending_ and
    // writes it out; the handshake step queues the engine's output itself.
    fiber::common::IoErr flush_output(fiber::event::IoEvent &event) noexcept;
    // Connected-phase flush: None once everything is on the wire; WouldBlock
    // once the rest is handed to the drain; anything else is latched by
    // fail_write.
    fiber::common::IoErr flush_connected() noexcept;
    // Latches a write error: the stream's integrity is gone, and the output
    // that never made it out is dropped with the drain.
    void fail_write(fiber::common::IoErr err) noexcept;
    // This object's StreamFd write subscription, held while a drain is in
    // flight or a TLS-level write subscriber exists.
    fiber::common::IoErr subscribe_stream_write() noexcept;
    void unsubscribe_stream_write() noexcept;
    // The StreamFd write subscription (ctx: this): continues the drain, then
    // notifies the TLS-level subscriber once nothing is left.
    static void on_stream_writable(void *ctx, fiber::common::IoErr err) noexcept;
    void handle_stream_writable() noexcept;
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
    // The TLS-level write subscriber (a callback, or a WaitWritableAwaiter).
    ReadyCallback write_callback_ = nullptr;
    void *write_callback_ctx_ = nullptr;
    // A connected-phase flush left out_pending_ to the drain; once connected,
    // out_pending_ is non-empty exactly while this is set.
    bool draining_ = false;
    // on_stream_writable is installed on stream_fd_'s write direction.
    bool write_subscribed_ = false;
    // NoMem while sealing a batch (records may sit sealed but unreported) or
    // a failed flush: the stream's integrity is gone — latched until close().
    fiber::common::IoErr write_error_ = fiber::common::IoErr::None;
    bool handshake_started_ = false;
    bool handshake_done_ = false;
    bool shutdown_started_ = false;
    bool busy_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_STREAM_FD_H
