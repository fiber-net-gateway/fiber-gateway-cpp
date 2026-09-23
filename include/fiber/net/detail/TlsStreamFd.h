#ifndef FIBER_NET_DETAIL_TLS_STREAM_FD_H
#define FIBER_NET_DETAIL_TLS_STREAM_FD_H

#include <chrono>
#include <cstddef>
#include <string_view>

#include "../../async/Task.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
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
    using ShutdownTask = fiber::async::Task<fiber::common::IoResult<void>>;
    using IoTask = fiber::async::Task<fiber::common::IoResult<size_t>>;
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

    [[nodiscard]] IoTask read(void *buf, size_t len,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask write(const void *buf, size_t len,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] HandshakeTask handshake(const TlsClientParam &param,
                                          std::chrono::milliseconds timeout = kDefaultTlsHandshakeTimeout);
    [[nodiscard]] HandshakeTask handshake(const TlsServerParam &param,
                                          std::chrono::milliseconds timeout = kDefaultTlsHandshakeTimeout);
    [[nodiscard]] ShutdownTask shutdown();
    [[nodiscard]] StreamFd::WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] StreamFd::WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::common::IoErr poll_shutdown(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_read(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_write(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;

private:
    enum class Role : std::uint8_t {
        None,
        Client,
        Server,
    };

    // Handshake staging + engines, heap-allocated per connection (defined in
    // the .cpp): staged configs borrow the caller's param material under the
    // documented param contract (valid until the handshake co_returns).
    struct Handshake;

    common::IoResult<void> start_client(const TlsClientParam &param) noexcept;
    common::IoResult<void> start_server(const TlsServerParam &param) noexcept;
    // TlsServerConfigSource::select — re-stages the server config per
    // ClientHello through the param's configure callback.
    static const tls::TlsServerConfig *select_server_config(void *ctx,
                                                            const tls::TlsClientHello &client_hello) noexcept;
    [[nodiscard]] HandshakeTask handshake_impl(common::IoResult<void> start_result, std::chrono::milliseconds timeout);
    fiber::common::IoErr handshake_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr shutdown_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    // Moves engine/connection output into out_pending_ and writes it out.
    fiber::common::IoErr flush_output(fiber::event::IoEvent &event) noexcept;
    // Reads the fd to drain and feeds the live engine (handshake phase).
    fiber::common::IoErr feed_engine(fiber::event::IoEvent &event) noexcept;

    StreamFd stream_fd_;
    // Private per-connection node allocator (pure freelist, loop-independent)
    // shared by the handshake engines and TlsConnection; destroyed after the
    // chains that borrow it.
    mem::IoBufNodePool *pool_ = nullptr;
    Handshake *hs_ = nullptr; // live until the handshake completes/fails
    tls::TlsConnection *conn_ = nullptr; // the connected phase
    mem::IoBufChain out_pending_{}; // sealed records not yet on the wire
    mem::IoBufChain early_data_{}; // server: decrypted 0-RTT, delivered first
    // Retry-contract identity of the payload sealed into out_pending_ (the
    // caller must retry poll_write with the same buffer until completion).
    const void *pending_write_ptr_ = nullptr;
    size_t pending_write_len_ = 0;
    Role role_ = Role::None;
    bool handshake_done_ = false;
    bool shutdown_started_ = false;
    bool busy_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_STREAM_FD_H
