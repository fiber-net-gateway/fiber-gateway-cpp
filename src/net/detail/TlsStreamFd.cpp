#include <fiber/net/detail/TlsStreamFd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <sys/uio.h>
#include <type_traits>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>
#include <fiber/net/detail/TlsClientStaging.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsTicketService.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordFramer.h>
#include "tls/handshake/TlsClientHandshakeEngine.h"
#include "tls/handshake/TlsServerHandshakeEngine.h"

namespace fiber::net::detail {

namespace {

using Deadline = std::chrono::steady_clock::time_point;

Deadline make_deadline(std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return Deadline::max();
    }
    return fiber::event::EventLoop::current().now() + timeout;
}

fiber::common::IoResult<std::chrono::milliseconds> remaining_timeout(Deadline deadline) noexcept {
    if (deadline == Deadline::max()) {
        return std::chrono::milliseconds::max();
    }
    auto now = fiber::event::EventLoop::current().now();
    if (deadline <= now) {
        return std::unexpected(fiber::common::IoErr::TimedOut);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds::zero()) {
        remaining = std::chrono::milliseconds(1);
    }
    return remaining;
}

struct BusyResetGuard {
    bool *busy = nullptr;

    explicit BusyResetGuard(bool *value) noexcept : busy(value) {}

    BusyResetGuard(const BusyResetGuard &) = delete;
    BusyResetGuard &operator=(const BusyResetGuard &) = delete;

    ~BusyResetGuard() {
        if (busy) {
            *busy = false;
        }
    }
};

// Wire-read chunk for the handshake engine feeds: large enough that a full
// flight lands in one try_read, one node's worth of memory.
constexpr std::size_t kReadChunk = 32 * 1024;
// Connected-phase wire buffer (feature/tls/12 §2): sized from the caller's
// read size plus record overhead, clamped to [min, max]. The minimum holds
// the largest record the framer accepts, so a carried incomplete record
// always completes in the next buffer; both bounds leave room for IoBuf's
// control block so the allocation lands exactly on a size class.
constexpr std::size_t kInboundMaxCapacity = 64 * 1024 - mem::kIoBufControlBlockSize;
constexpr std::size_t kInboundMinCapacity = 20 * 1024 - mem::kIoBufControlBlockSize;
constexpr std::size_t kInboundSlack = 200; // ~9 TLS 1.3 records' header + tag + type
static_assert(kInboundMinCapacity >= tls::kTlsRecordHeaderSize + tls::kTlsMaxCiphertextRecordSize);
constexpr int kMaxIov = 16;
// One TLS record's maximum plaintext — the write-side grouping granularity.
constexpr std::size_t kRecordPlaintextMax = tls::kTlsMaxPlaintextSize;
// Plaintext sealed per try_write before the one flush (feature/tls/13): ~4
// records, so a chain of small nodes (H2's 9-byte frame headers between
// payloads) costs one sendmsg per batch instead of one per record.
constexpr std::size_t kWriteBatchBytes = 64 * 1024;

// Read-only walk over a chain's readable bytes: the current node and the
// offset into its readable region. Empty nodes are skipped.
class ChainCursor {
public:
    explicit ChainCursor(const mem::IoBufChain &chain) noexcept : node_(chain.front_node()) { skip_empty(); }

    [[nodiscard]] bool at_end() const noexcept { return node_ == nullptr; }

    // The rest of the current node.
    [[nodiscard]] std::span<const std::uint8_t> segment() const noexcept {
        return {node_->buf.readable_data() + offset_, node_->buf.readable() - offset_};
    }

    // No readable byte follows the current segment.
    [[nodiscard]] bool last_segment() const noexcept {
        for (const mem::IoBufNode *node = node_->next; node != nullptr; node = node->next) {
            if (node->buf.readable() > 0) {
                return false;
            }
        }
        return true;
    }

    // Copies up to dst.size() bytes from the cursor on, across nodes; the
    // cursor itself does not move. Returns the bytes copied.
    std::size_t copy_out(std::span<std::uint8_t> dst) const noexcept {
        std::size_t copied = 0;
        std::size_t offset = offset_;
        for (const mem::IoBufNode *node = node_; node != nullptr && copied < dst.size(); node = node->next) {
            const std::size_t take = std::min(node->buf.readable() - offset, dst.size() - copied);
            if (take > 0) {
                std::memcpy(dst.data() + copied, node->buf.readable_data() + offset, take);
                copied += take;
            }
            offset = 0;
        }
        return copied;
    }

    // Moves past `bytes` readable bytes (at most what remains).
    void advance(std::size_t bytes) noexcept {
        while (bytes > 0) {
            const std::size_t left = node_->buf.readable() - offset_;
            if (bytes < left) {
                offset_ += bytes;
                return;
            }
            bytes -= left;
            node_ = node_->next;
            offset_ = 0;
            skip_empty();
        }
    }

private:
    void skip_empty() noexcept {
        while (node_ != nullptr && node_->buf.readable() == 0) {
            node_ = node_->next;
        }
    }

    const mem::IoBufNode *node_ = nullptr;
    std::size_t offset_ = 0;
};

// Total size of the incomplete record heading `buf`: header plus body once
// the header is in (process_inbound left it validated), else just the header.
std::size_t incomplete_record_size(const mem::IoBuf &buf) noexcept {
    if (buf.readable() < tls::kTlsRecordHeaderSize) {
        return tls::kTlsRecordHeaderSize;
    }
    const std::uint8_t *header = buf.readable_data();
    return tls::kTlsRecordHeaderSize + ((static_cast<std::size_t>(header[3]) << 8) | header[4]);
}

bool version_bounds_ok(int min_version, int max_version) noexcept {
    const auto in_domain = [](int version) noexcept { return version == 0x0303 || version == 0x0304; };
    return in_domain(min_version) && in_domain(max_version) && min_version <= max_version;
}

// Per-ClientHello server config selection (09 §4.1): the
// TlsServerConfigSource ctx, a frame local of the server handshake declared
// ahead of the engine that borrows it. cfg starts as the template staged from
// the param; select_server_config re-stages it through the param's configure
// callback and latches the callback's error for the handshake to report after
// flushing the fatal alert. A credential the callback hands over through
// add_credential lands in credential_owner, which is destroyed after the
// engine (reverse declaration order) — the engine may read its chain/key
// until then.
struct ServerSelection {
    const TlsServerParam &param;
    tls::TlsServerConfig cfg{};
    TlsCredential credential_owner;
    common::IoErr callback_error = common::IoErr::None;
};

} // namespace

TlsStreamFd::TlsStreamFd(fiber::event::EventLoop &loop, int fd) : stream_fd_(loop, fd) {}

TlsStreamFd::~TlsStreamFd() {
    if (!stream_fd_.valid() && !conn_) {
        return;
    }
    if (loop().in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

bool TlsStreamFd::valid() const noexcept { return stream_fd_.valid(); }

int TlsStreamFd::fd() const noexcept { return stream_fd_.fd(); }

fiber::event::EventLoop &TlsStreamFd::loop() const noexcept { return stream_fd_.loop(); }

std::string_view TlsStreamFd::selected_alpn() const noexcept {
    if (!conn_) {
        return {};
    }
    const std::span<const std::uint8_t> proto = conn_->alpn();
    if (proto.empty()) {
        return {};
    }
    return {reinterpret_cast<const char *>(proto.data()), proto.size()};
}

bool TlsStreamFd::handshake_done() const noexcept { return handshake_done_; }

bool TlsStreamFd::has_pending_read() const noexcept {
    if (!conn_) {
        return false;
    }
    // Terminals included: a latched peer close_notify (EOF) or fatal is a read
    // result that no socket byte will ever announce — the peer may have sent
    // its last record in a feed we already consumed. Without this the
    // transport's wait_readable would block on an edge that already fired.
    return !early_data_.empty() || conn_->pending_plaintext() > 0 || conn_->peer_closed() || conn_->failed();
}

void TlsStreamFd::close() {
    FIBER_ASSERT(loop().in_loop());
    if (conn_) {
        // After a latched write error the output may hold sealed records the
        // caller never saw written: send nothing — no close_notify that
        // would pass the truncated stream off as complete.
        if (write_error_ == fiber::common::IoErr::None) {
            if (!conn_->failed() && !conn_->peer_closed()) {
                (void) conn_->close_notify();
            }
            fiber::event::IoEvent event = fiber::event::IoEvent::None;
            (void) flush_output(event); // best-effort single drain; fd may be gone
        }
        conn_.reset();
    }
    // The write side detaches before the fd closes: the drain stops here, and
    // the TLS-level write subscriber completes last, from locals — the
    // completions inside stream_fd_.close() may destroy this object, so
    // every member is reset ahead of it.
    unsubscribe_stream_write();
    draining_ = false;
    const ReadyCallback write_callback = std::exchange(write_callback_, nullptr);
    void *const write_callback_ctx = std::exchange(write_callback_ctx_, nullptr);
    inbound_ = mem::IoBuf{};
    // Output the drain above left behind (a close_notify a gone peer refused,
    // a batch stuck on WouldBlock) and unread 0-RTT go back to this loop's
    // node pool now: a closed stream may then be destroyed off the loop.
    out_pending_.clear();
    early_data_.clear();
    handshake_started_ = false;
    handshake_done_ = false;
    shutdown_started_ = false;
    busy_ = false;
    write_error_ = fiber::common::IoErr::None;
    // A mid-handshake close touches no handshake state here: the fd close
    // below wakes the suspended handshake with Canceled (inline resume), it
    // unwinds, and its frame-local staging — the engine included — dies with
    // the coroutine frame on this loop.
    if (stream_fd_.valid()) {
        stream_fd_.close();
    }
    if (write_callback != nullptr) {
        write_callback(write_callback_ctx, fiber::common::IoErr::Canceled);
    }
}

fiber::common::IoErr TlsStreamFd::detach_for_handover() noexcept {
    FIBER_ASSERT(!busy_);
    // Only writev users hand over (the H1 pools), and writev returns once its
    // batch is on the wire: nothing may still be draining.
    FIBER_ASSERT(out_pending_.empty());
    FIBER_ASSERT(write_callback_ == nullptr && !write_subscribed_);
    return stream_fd_.detach_for_handover();
}

fiber::common::IoErr TlsStreamFd::adopt_loop(fiber::event::EventLoop &loop) noexcept {
    return stream_fd_.adopt_loop(loop);
}

fiber::common::IoErr TlsStreamFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    // RWFd asserts the same of a Ready fd; here the subscription would simply
    // never fire, the plaintext's wire edge having fired already.
    FIBER_ASSERT(!has_pending_read());
    return stream_fd_.set_read_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!callback) {
        return fiber::common::IoErr::Invalid;
    }
    // RWFd's contract on the TLS write direction: a write_ready() caller
    // advances by writing. Not ready means a drain in flight (already
    // subscribed) or a socket that is not Ready (accepts the subscription).
    FIBER_ASSERT(!write_ready());
    if (write_callback_ != nullptr) {
        return fiber::common::IoErr::Busy;
    }
    const fiber::common::IoErr err = subscribe_stream_write();
    if (err != fiber::common::IoErr::None) {
        return err;
    }
    write_callback_ = callback;
    write_callback_ctx_ = ctx;
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::set_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.set_terminal_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_read_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(loop().in_loop());
    if (!callback) {
        return fiber::common::IoErr::Invalid;
    }
    if (write_callback_ != callback || write_callback_ctx_ != ctx) {
        return fiber::common::IoErr::None;
    }
    write_callback_ = nullptr;
    write_callback_ctx_ = nullptr;
    if (!draining_) {
        unsubscribe_stream_write(); // a drain in flight keeps its subscription
    }
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::subscribe_stream_write() noexcept {
    if (write_subscribed_) {
        return fiber::common::IoErr::None;
    }
    const fiber::common::IoErr err = stream_fd_.set_write_callback(&TlsStreamFd::on_stream_writable, this);
    if (err == fiber::common::IoErr::None) {
        write_subscribed_ = true;
    }
    return err;
}

void TlsStreamFd::unsubscribe_stream_write() noexcept {
    if (!write_subscribed_) {
        return;
    }
    (void) stream_fd_.clear_write_callback(&TlsStreamFd::on_stream_writable, this);
    write_subscribed_ = false;
}

void TlsStreamFd::on_stream_writable(void *ctx, fiber::common::IoErr err) noexcept {
    // close() unsubscribes before it closes the fd, and a handover requires
    // no subscription: only a readiness transition arrives here.
    FIBER_ASSERT(err == fiber::common::IoErr::None);
    static_cast<TlsStreamFd *>(ctx)->handle_stream_writable();
}

void TlsStreamFd::handle_stream_writable() noexcept {
    if (draining_) {
        fiber::event::IoEvent event = fiber::event::IoEvent::None;
        const fiber::common::IoErr err = flush_output(event);
        if (err == fiber::common::IoErr::WouldBlock) {
            return; // still subscribed: the next transition continues the drain
        }
        draining_ = false;
        if (err != fiber::common::IoErr::None) {
            fail_write(err);
        }
    }
    if (write_callback_ == nullptr) {
        unsubscribe_stream_write();
        return;
    }
    // Drained (or failed: the subscriber's next write reports it). Last: the
    // subscriber may write, re-subscribe or clear from here, and the transport
    // contract keeps this object alive until dispatch returns.
    write_callback_(write_callback_ctx_, fiber::common::IoErr::None);
}

void TlsStreamFd::fail_write(fiber::common::IoErr err) noexcept {
    write_error_ = err;
    draining_ = false;
    out_pending_.clear();
}

fiber::common::IoErr TlsStreamFd::flush_connected() noexcept {
    fiber::event::IoEvent event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = flush_output(event);
    if (err == fiber::common::IoErr::None) {
        if (draining_) {
            // A direct flush (poll_shutdown) finished a drain in flight.
            draining_ = false;
            if (write_callback_ == nullptr) {
                unsubscribe_stream_write();
            }
        }
        return err;
    }
    if (err == fiber::common::IoErr::WouldBlock) {
        // The socket just went Blocked, so the StreamFd takes the
        // subscription; the drain owns the rest from here.
        err = subscribe_stream_write();
        if (err == fiber::common::IoErr::None) {
            draining_ = true;
            return fiber::common::IoErr::WouldBlock;
        }
    }
    fail_write(err);
    if (write_callback_ == nullptr) {
        unsubscribe_stream_write();
    }
    return err;
}

fiber::common::IoErr TlsStreamFd::clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_terminal_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::check_handshake_start(int min_version, int max_version) const noexcept {
    if (handshake_started_) {
        return fiber::common::IoErr::Already;
    }
    if (!stream_fd_.valid()) {
        return fiber::common::IoErr::BadFd;
    }
    if (!version_bounds_ok(min_version, max_version)) {
        return fiber::common::IoErr::Invalid;
    }
    return fiber::common::IoErr::None;
}

const tls::TlsServerConfig *TlsStreamFd::select_server_config(void *ctx,
                                                              const tls::TlsClientHello &client_hello) noexcept {
    auto *selection = static_cast<ServerSelection *>(ctx);
    selection->callback_error = common::IoErr::None;
    selection->cfg.chain = nullptr;
    selection->cfg.key = nullptr;
    selection->credential_owner.reset();
    std::size_t credential_count = 0;
    TlsServerHandshakeConfig config(selection->cfg, credential_count, selection->credential_owner);
    const tls::TlsClientHelloView view = client_hello.view();
    common::IoErr error = selection->param.configure_callback(selection->param.configure_ctx, config, view);
    if (error == common::IoErr::None && credential_count == 0) {
        error = common::IoErr::Invalid;
    }
    if (error != common::IoErr::None) {
        selection->credential_owner.reset(); // nothing will read it: release now
        selection->callback_error = error;
        return nullptr; // the engine answers handshake_failure
    }
    return &selection->cfg;
}

TlsStreamFd::HandshakeTask TlsStreamFd::handshake(const TlsClientParam &param, std::chrono::milliseconds timeout) {
    if (busy_) {
        co_return std::unexpected(fiber::common::IoErr::Busy);
    }
    busy_ = true;
    BusyResetGuard busy_reset(&busy_);

    if (const fiber::common::IoErr err = check_handshake_start(param.min_version, param.max_version);
        err != fiber::common::IoErr::None) {
        co_return std::unexpected(err);
    }
    // Frame-local staging: the engine copies cfg, but cfg.verify_ip keeps
    // borrowing ip_bytes, which therefore outlives the engine.
    tls::TlsClientConfig cfg{};
    std::array<std::uint8_t, 16> ip_bytes{};
    if (auto staged = TlsClientStager::stage(param, cfg, ip_bytes); !staged) {
        co_return std::unexpected(staged.error());
    }
    cfg.min_version = static_cast<std::uint16_t>(param.min_version);
    cfg.max_version = static_cast<std::uint16_t>(param.max_version);
    // A construction failure (entropy/allocation) is already terminal with
    // any alert encoded — the first step flushes it, then reports.
    tls::TlsClientHandshakeEngine engine(cfg, nullptr);
    handshake_started_ = true;

    const Deadline deadline = make_deadline(timeout);
    for (;;) {
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        const fiber::common::IoErr err = handshake_step(engine, wait_event);
        if (err == fiber::common::IoErr::None) {
            co_return fiber::common::IoResult<void>{};
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        fiber::common::IoResult<void> waited{};
        if (wait_event == fiber::event::IoEvent::Read) {
            waited = co_await stream_fd_.wait_readable(*remaining);
        } else {
            waited = co_await stream_fd_.wait_writable(*remaining);
        }
        if (!waited) {
            co_return std::unexpected(waited.error());
        }
    }
}

TlsStreamFd::HandshakeTask TlsStreamFd::handshake(const TlsServerParam &param, std::chrono::milliseconds timeout) {
    if (busy_) {
        co_return std::unexpected(fiber::common::IoErr::Busy);
    }
    busy_ = true;
    BusyResetGuard busy_reset(&busy_);

    if (const fiber::common::IoErr err = check_handshake_start(param.min_version, param.max_version);
        err != fiber::common::IoErr::None) {
        co_return std::unexpected(err);
    }
    if (!param.enabled()) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    // Client-certificate verification needs a trust store up front.
    if (param.client_certificate_mode != TlsClientCertificateMode::None && param.trust_store == nullptr) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }

    // Frame-local staging, declared ahead of the engine that borrows all of
    // it: the selection, the ticket adapters and the source die after it.
    ServerSelection selection{param};
    tls::TlsServerConfig &cfg = selection.cfg;
    if (param.trust_store != nullptr) {
        cfg.client_trust = &param.trust_store->tls_store();
    }
    cfg.require_client_cert = param.client_certificate_mode == TlsClientCertificateMode::Required;
    cfg.alpn = param.alpn;
    cfg.min_version = static_cast<std::uint16_t>(param.min_version);
    cfg.max_version = static_cast<std::uint16_t>(param.max_version);
    cfg.enable_early_data = param.enable_early_data;
    cfg.now_unix_ms = system_now_unix_ms();
    // Session tickets (09 §6): the service's adapters are staged here so the
    // engine borrows them for the handshake. Unconfigured stays null — no NST,
    // no resumption (decision 1) — which the engine treats natively.
    const bool tickets = param.ticket_service != nullptr;
    tls::TlsTicketMinter minter{};
    tls::TlsResumptionLookup lookup{};
    if (tickets) {
        minter = param.ticket_service->minter();
        lookup = param.ticket_service->lookup();
    }
    tls::TlsServerConfigSource source{};
    source.select = &TlsStreamFd::select_server_config;
    source.ctx = &selection;
    // Credentials arrive per ClientHello through the source, so the template
    // config passes only the selector-mode invariant checks.
    tls::TlsServerHandshakeEngine engine(cfg, tickets ? &lookup : nullptr, tickets ? &minter : nullptr, &source);
    handshake_started_ = true;

    const Deadline deadline = make_deadline(timeout);
    for (;;) {
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        const fiber::common::IoErr err = handshake_step(engine, wait_event);
        if (err == fiber::common::IoErr::None) {
            co_return fiber::common::IoResult<void>{};
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            // A failed engine's report yields to the configure callback's
            // latched error (the alert is already on the wire).
            if (err == fiber::common::IoErr::Invalid && engine.failed() &&
                selection.callback_error != common::IoErr::None) {
                co_return std::unexpected(selection.callback_error);
            }
            co_return std::unexpected(err);
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        fiber::common::IoResult<void> waited{};
        if (wait_event == fiber::event::IoEvent::Read) {
            waited = co_await stream_fd_.wait_readable(*remaining);
        } else {
            waited = co_await stream_fd_.wait_writable(*remaining);
        }
        if (!waited) {
            co_return std::unexpected(waited.error());
        }
    }
}

fiber::common::IoResult<bool> TlsStreamFd::on_read_wait_gate(void *ctx, fiber::event::IoEvent direction) noexcept {
    auto *self = static_cast<TlsStreamFd *>(ctx);
    // Ahead of the stream's gate: like a TCP receive queue, the plaintext
    // drains before a recorded socket error is reported.
    if (self->has_pending_read()) {
        return true;
    }
    return self->stream_fd_.stream_wait_gate()(direction);
}

StreamFd::WaitReadableAwaiter TlsStreamFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return stream_fd_.rwfd().wait_readable(timeout, {&TlsStreamFd::on_read_wait_gate, this});
}

TlsStreamFd::WaitWritableAwaiter TlsStreamFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return WaitWritableAwaiter(*this, timeout);
}

TlsStreamFd::WaitWritableAwaiter::WaitWritableAwaiter(TlsStreamFd &stream, std::chrono::milliseconds timeout) noexcept :
    stream_(&stream), timeout_(timeout) {}

TlsStreamFd::WaitWritableAwaiter::~WaitWritableAwaiter() {
    cancel_timer();
    if (waiting_) {
        // Abandoned while suspended (timeout_for, frame destruction): drop the
        // subscription, never resume from here.
        waiting_ = false;
        (void) stream_->clear_write_callback(&WaitWritableAwaiter::on_ready, this);
    }
}

bool TlsStreamFd::WaitWritableAwaiter::await_ready() noexcept {
    if (timeout_ > std::chrono::milliseconds::zero()) {
        return false;
    }
    err_ = fiber::common::IoErr::TimedOut;
    return true;
}

bool TlsStreamFd::WaitWritableAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    FIBER_ASSERT(stream_->loop().in_loop());
    loop_ = &stream_->loop();
    coro_ = handle;
    // The stream's veto first: a terminal fd completes the wait with its error.
    const auto gated = stream_->stream_fd_.stream_wait_gate()(fiber::event::IoEvent::Write);
    if (!gated) {
        err_ = gated.error();
        return false;
    }
    if (*gated || stream_->write_ready()) {
        // Ready (a latched write error included): write instead of waiting.
        err_ = fiber::common::IoErr::None;
        return false;
    }
    const fiber::common::IoErr installed = stream_->set_write_callback(&WaitWritableAwaiter::on_ready, this);
    if (installed != fiber::common::IoErr::None) {
        err_ = installed;
        return false;
    }
    waiting_ = true;
    if (timeout_ != std::chrono::milliseconds::max()) {
        loop_->post_at<WaitWritableAwaiter, &WaitWritableAwaiter::timer_entry_, &WaitWritableAwaiter::on_timeout>(
                loop_->now() + timeout_, *this);
    }
    return true;
}

fiber::common::IoResult<void> TlsStreamFd::WaitWritableAwaiter::await_resume() noexcept {
    waiting_ = false;
    cancel_timer();
    if (err_ == fiber::common::IoErr::None) {
        return {};
    }
    return std::unexpected(err_);
}

void TlsStreamFd::WaitWritableAwaiter::on_ready(void *ctx, fiber::common::IoErr err) noexcept {
    auto *awaiter = static_cast<WaitWritableAwaiter *>(ctx);
    FIBER_ASSERT(awaiter->waiting_);
    awaiter->waiting_ = false;
    if (err != fiber::common::IoErr::Canceled) {
        // One-shot over a persistent slot: unsubscribe before resuming.
        // Canceled comes from close(), which has emptied the slot already.
        (void) awaiter->stream_->clear_write_callback(&WaitWritableAwaiter::on_ready, awaiter);
    }
    awaiter->err_ = err;
    awaiter->cancel_timer();
    awaiter->coro_.resume();
}

void TlsStreamFd::WaitWritableAwaiter::on_timeout(WaitWritableAwaiter *awaiter) noexcept {
    FIBER_ASSERT(awaiter->waiting_);
    awaiter->waiting_ = false;
    awaiter->err_ = fiber::common::IoErr::TimedOut;
    (void) awaiter->stream_->clear_write_callback(&WaitWritableAwaiter::on_ready, awaiter);
    awaiter->coro_.resume();
}

void TlsStreamFd::WaitWritableAwaiter::cancel_timer() noexcept {
    if (!timer_entry_.is_in_heap()) {
        return;
    }
    FIBER_ASSERT(loop_ != nullptr && loop_->in_loop());
    loop_->cancel<WaitWritableAwaiter, &WaitWritableAwaiter::timer_entry_>(*this);
}

fiber::common::IoErr TlsStreamFd::poll_shutdown(fiber::event::IoEvent &event) noexcept { return shutdown_once(event); }

fiber::common::IoResult<std::size_t> TlsStreamFd::try_read(std::size_t size, mem::IoBufChain &out) noexcept {
    if (!stream_fd_.valid() || !conn_) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (size == 0) {
        return std::size_t{0};
    }
    // The wire read is sized from what the caller asked for; delivery stays
    // capped at a record's worth of plaintext per call (the take moves
    // retained views, zero-copy).
    const std::size_t wire_hint = size;
    size = std::min(size, kRecordPlaintextMax);
    for (;;) {
        if (!early_data_.empty()) {
            // Server 0-RTT: decrypted early data delivers before anything
            // decrypted under the handshake keys.
            const std::size_t readable = early_data_.readable_bytes();
            const std::size_t bytes = readable < size ? readable : size;
            if (!early_data_.take_prefix(bytes, out)) {
                return std::unexpected(fiber::common::IoErr::NoMem);
            }
            return bytes;
        }
        std::size_t got = 0;
        const auto status = conn_->take(size, out, got);
        switch (status) {
            case tls::TlsConnection::ReadStatus::Ok:
                return got;
            case tls::TlsConnection::ReadStatus::PeerClosed:
                return std::size_t{0}; // EOF (close_notify), out = 0
            case tls::TlsConnection::ReadStatus::Fatal:
                return std::unexpected(fiber::common::IoErr::Invalid);
            case tls::TlsConnection::ReadStatus::NoMem:
                return std::unexpected(fiber::common::IoErr::NoMem);
            case tls::TlsConnection::ReadStatus::NeedMore:
                break;
        }
        // No plaintext buffered: pull wire bytes and open their records.
        const fiber::common::IoErr err = read_wire(wire_hint);
        if (err != fiber::common::IoErr::None) {
            return std::unexpected(err); // WouldBlock included: the socket read is the only stall
        }
    }
}

fiber::async::Task<fiber::common::IoResult<std::size_t>> TlsStreamFd::readv(std::size_t size, mem::IoBufChain &out,
                                                                            std::chrono::milliseconds timeout) {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto read = try_read(size, out);
        if (read) {
            // One node of plaintext per call — the caller drives the loop.
            co_return read;
        }
        if (read.error() != fiber::common::IoErr::WouldBlock) {
            co_return std::unexpected(read.error());
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await stream_fd_.wait_readable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

template<class Engine>
fiber::common::IoErr TlsStreamFd::handshake_step(Engine &engine, fiber::event::IoEvent &event) noexcept {
    constexpr bool kServer = std::is_same_v<Engine, tls::TlsServerHandshakeEngine>;
    if (!stream_fd_.valid()) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        // Flights and alerts go out before anything else — including the
        // fatal alert that ends a failed handshake (the failure return only
        // happens once the wire is clean). Once connected, the engine is
        // spent and flush_output queues the connection's output instead.
        if (!conn_) {
            FIBER_ASSERT(out_pending_.append_chain(engine.take_output()));
        }
        fiber::common::IoErr err = flush_output(event);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        // The success path falls through to this check on its next pass, and
        // a step resumed after a WouldBlock tail flush lands here too: tail
        // output flushed, then done (a leftover-fed record violation
        // surfacing as conn_->failed() reports Invalid here).
        if (handshake_done_) {
            return conn_->failed() ? fiber::common::IoErr::Invalid : fiber::common::IoErr::None;
        }

        if (!engine.done()) {
            mem::IoBuf chunk;
            err = read_handshake_chunk(chunk, event);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
            auto fed = engine.feed(std::move(chunk));
            if (!fed) {
                return fed.error();
            }
            continue;
        }
        if (engine.failed()) {
            return fiber::common::IoErr::Invalid; // the alert (if any) is on the wire per the flush above
        }

        // Success: swap the handshake engine for the connected-phase engine.
        // The flush above already moved the engine's final output (a TLS 1.3
        // client's Finished, sealed by the last feed) into out_pending_.
        tls::TlsConnectedState state = engine.take_state();
        mem::IoBufChain leftover = engine.take_inbound_leftover();
        if constexpr (kServer) {
            early_data_ = engine.take_early_data();
        }
        err = install_connection(kServer ? tls::TlsConnectionRole::Server : tls::TlsConnectionRole::Client,
                                 std::move(state), std::move(leftover));
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        handshake_done_ = true;
    }
}

fiber::common::IoErr TlsStreamFd::install_connection(tls::TlsConnectionRole role, tls::TlsConnectedState &&state,
                                                     mem::IoBufChain &&leftover) noexcept {
    conn_.emplace(role, std::move(state));
    // The leftover bytes — records past the final flight (app data
    // piggybacked behind it) — become the first wire buffer. Usually one view
    // of the engine's last read chunk, framed in place; otherwise gathered
    // once so every record stays contiguous.
    const std::size_t bytes = leftover.readable_bytes();
    if (bytes == 0) {
        return fiber::common::IoErr::None;
    }
    const mem::IoBuf *front = leftover.first_readable();
    if (front->readable() == bytes) {
        inbound_ = *front;
    } else {
        inbound_ = mem::IoBuf::allocate(bytes);
        if (!inbound_.valid()) {
            return fiber::common::IoErr::NoMem;
        }
        for (const mem::IoBufNode *node = leftover.front_node(); node != nullptr; node = node->next) {
            std::memcpy(inbound_.writable_data(), node->buf.readable_data(), node->buf.readable());
            inbound_.commit(node->buf.readable());
        }
    }
    process_inbound();
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::shutdown_once(fiber::event::IoEvent &event) noexcept {
    if (!stream_fd_.valid()) {
        return fiber::common::IoErr::BadFd;
    }
    if (!handshake_done_ || !conn_) {
        // Nothing to close gracefully (never started / mid-handshake).
        return fiber::common::IoErr::None;
    }
    if (write_error_ != fiber::common::IoErr::None) {
        return write_error_; // no graceful close for a stream that lost integrity
    }
    if (!shutdown_started_) {
        auto closed = conn_->close_notify();
        if (!closed) {
            return closed.error();
        }
        shutdown_started_ = true;
    }
    // Send our close_notify — behind any output still draining — and be
    // done: waiting for the peer's echo is the reader's business (read_once
    // surfaces PeerClosed), not the closer's. WouldBlock: the drain sends the
    // rest, and write readiness reports when it has.
    const fiber::common::IoErr err = flush_connected();
    if (err == fiber::common::IoErr::WouldBlock) {
        event = fiber::event::IoEvent::Write;
    }
    return err;
}

// TLS writes seal a contiguous input buffer into records. Unlike sendmsg,
// there is no scatter-gather seal API, so multi-node chains would otherwise
// produce one record per IoBuf: an HTTP/2 DATA frame is a 9-byte header node
// followed by its payload node, and writing the header as its own record
// costs a record of overhead for 9 bytes. A node that is itself at least one
// full record is passed to the seal with the node's own pointer (zero copy).
// Anything smaller is coalesced into a scratch buffer with the nodes that
// follow it, splitting the last node when needed, so that each scratch write
// is a full record whenever the chain holds enough data. Groups seal back to
// back into one batch (feature/tls/13) and the batch takes one flush: one
// sendmsg per ~64 KiB instead of one per record.
fiber::common::IoErr TlsStreamFd::seal_write_batch(const mem::IoBufChain &buf, std::size_t &batch_len) noexcept {
    ChainCursor cursor(buf);
    FIBER_ASSERT(!cursor.at_end()); // readable_bytes() > 0 with no pending batch
    // Coalescing scratch on the stack: a group is copied in and sealed out
    // before the next one starts, and nothing of it outlives this call (the
    // retry state is the sealed records), so the thread's stack serves every
    // connection — no per-connection 16 KiB, no allocation, always hot.
    // Uninitialized on purpose: only the copied prefix is ever read.
    std::array<std::uint8_t, kRecordPlaintextMax> scratch;
    while (batch_len < kWriteBatchBytes && !cursor.at_end()) {
        // Zero-copy groups take whole records, at least one, within the
        // batch's remaining room.
        const std::size_t room = kWriteBatchBytes - batch_len;
        const std::size_t record_room = std::max(kRecordPlaintextMax, room - room % kRecordPlaintextMax);
        const std::span<const std::uint8_t> segment = cursor.segment();
        std::span<const std::uint8_t> group;
        if (cursor.last_segment()) {
            group = segment.first(std::min(segment.size(), record_room)); // a short final record is fine
        } else if (segment.size() >= kRecordPlaintextMax) {
            // Whole records straight from the node; its tail joins the next
            // group so it does not become a short record of its own.
            group = segment.first(std::min(segment.size() - segment.size() % kRecordPlaintextMax, record_room));
        } else {
            group = {scratch.data(), cursor.copy_out(scratch)};
        }
        auto sealed = conn_->write(group);
        if (!sealed) {
            // Invalid (a terminal connection) only ever fails the first group,
            // before anything is sealed. NoMem may leave records of this or
            // earlier groups sealed but unreported: latch it.
            if (sealed.error() == fiber::common::IoErr::NoMem) {
                write_error_ = fiber::common::IoErr::NoMem;
            }
            return sealed.error();
        }
        cursor.advance(group.size());
        batch_len += group.size();
    }
    return fiber::common::IoErr::None;
}

fiber::common::IoResult<std::size_t> TlsStreamFd::try_write(mem::IoBufChain &buf) noexcept {
    if (!stream_fd_.valid() || !conn_) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (write_error_ != fiber::common::IoErr::None) {
        return std::unexpected(write_error_);
    }
    if (!out_pending_.empty()) {
        // An earlier batch is draining: the socket is its until it is out,
        // and write readiness reports when that is.
        return std::unexpected(fiber::common::IoErr::WouldBlock);
    }
    if (buf.readable_bytes() == 0) {
        return std::size_t{0};
    }

    std::size_t batch_len = 0;
    fiber::common::IoErr err = seal_write_batch(buf, batch_len);
    if (err != fiber::common::IoErr::None) {
        return std::unexpected(err); // terminal/closed (Invalid) or NoMem (latched)
    }
    err = flush_connected();
    if (err != fiber::common::IoErr::None && err != fiber::common::IoErr::WouldBlock) {
        return std::unexpected(err); // latched; the chain stays as it was
    }
    // Sealed is accepted: a remainder the socket did not take drains on its own.
    buf.consume_and_compact(batch_len);
    return batch_len;
}

fiber::async::Task<fiber::common::IoResult<std::size_t>> TlsStreamFd::writev(mem::IoBufChain &buf,
                                                                             std::chrono::milliseconds timeout) {
    Deadline deadline = make_deadline(timeout);
    std::optional<std::size_t> accepted;
    for (;;) {
        if (!accepted) {
            auto written = try_write(buf);
            if (written) {
                accepted = *written; // one batch per call — the caller drives the loop
            } else if (written.error() != fiber::common::IoErr::WouldBlock) {
                co_return std::unexpected(written.error());
            }
        }
        if (accepted) {
            // Reported once on the wire; a drain that failed after acceptance
            // fails the call.
            if (write_error_ != fiber::common::IoErr::None) {
                co_return std::unexpected(write_error_);
            }
            if (out_pending_.empty()) {
                co_return *accepted;
            }
        }
        // A connected-phase write only ever blocks on writability.
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await wait_writable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::common::IoErr TlsStreamFd::flush_output(fiber::event::IoEvent &event) noexcept {
    if (conn_) {
        FIBER_ASSERT(out_pending_.append_chain(conn_->take_output()));
    }
    while (!out_pending_.empty()) {
        struct iovec iov[kMaxIov];
        const int count = out_pending_.fill_write_iov(iov, kMaxIov);
        if (count <= 0) {
            break;
        }
        auto written = stream_fd_.try_writev(iov, count);
        if (!written) {
            if (written.error() == fiber::common::IoErr::WouldBlock) {
                event = fiber::event::IoEvent::Write;
                return fiber::common::IoErr::WouldBlock;
            }
            return written.error();
        }
        if (*written == 0) {
            return fiber::common::IoErr::BrokenPipe;
        }
        // consume_and_compact: plain consume() leaves drained nodes linked,
        // and empty() counts nodes — a zombie chain would wedge later
        // writes behind the same-pointer retry contract.
        out_pending_.consume_and_compact(*written);
    }
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::read_handshake_chunk(mem::IoBuf &chunk, fiber::event::IoEvent &event) noexcept {
    chunk = mem::IoBuf::allocate(kReadChunk);
    if (!chunk.valid()) {
        return fiber::common::IoErr::NoMem;
    }
    auto read_result = stream_fd_.try_read(chunk.writable_data(), chunk.writable());
    if (!read_result) {
        if (read_result.error() == fiber::common::IoErr::WouldBlock) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        return read_result.error();
    }
    if (*read_result == 0) {
        // Plaintext EOF mid-handshake (no close_notify yet).
        return fiber::common::IoErr::ConnReset;
    }
    chunk.commit(*read_result);
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::read_wire(std::size_t hint) noexcept {
    const std::size_t carry = inbound_.readable(); // at most one incomplete record
    if (carry > 0 && carry + inbound_.writable() >= incomplete_record_size(inbound_)) {
        // The record still fits the buffer it started in: continue it in
        // the tailroom — no copy, no allocation. A record trickling in over
        // many small segments stays here instead of re-copying its growing
        // prefix per read. The tailroom lies past every delivered slice, so
        // writing it touches no shared bytes.
        auto read_result = stream_fd_.try_read(inbound_.writable_data(), inbound_.writable());
        if (!read_result) {
            return read_result.error();
        }
        if (*read_result == 0) {
            return fiber::common::IoErr::ConnReset; // EOF without close_notify: truncation
        }
        inbound_.commit(*read_result);
        process_inbound();
        return fiber::common::IoErr::None;
    }
    mem::IoBuf wire = mem::IoBuf::allocate(std::clamp(hint + kInboundSlack, kInboundMinCapacity, kInboundMaxCapacity));
    if (!wire.valid()) {
        return fiber::common::IoErr::NoMem;
    }
    // Read past the carry slot first: a WouldBlock costs no carry copy and
    // leaves inbound_ as it was.
    auto read_result = stream_fd_.try_read(wire.writable_data() + carry, wire.writable() - carry);
    if (!read_result) {
        return read_result.error();
    }
    if (*read_result == 0) {
        // EOF without close_notify: truncation.
        return fiber::common::IoErr::ConnReset;
    }
    if (carry > 0) {
        std::memcpy(wire.writable_data(), inbound_.readable_data(), carry);
    }
    wire.commit(carry + *read_result);
    // The previous buffer lives on only through plaintext already delivered.
    inbound_ = std::move(wire);
    process_inbound();
    return fiber::common::IoErr::None;
}

void TlsStreamFd::process_inbound() noexcept {
    FIBER_ASSERT(conn_.has_value());
    std::array<tls::TlsRecordSpan, tls::kTlsRecordBatchMax> batch;
    for (;;) {
        const tls::TlsFrameResult framed =
                tls::tls_frame_records({inbound_.readable_data(), inbound_.readable()}, batch);
        if (framed.count > 0) {
            conn_->on_records(inbound_, {batch.data(), framed.count});
            inbound_.consume(framed.consumed);
        }
        // A terminal latched by the records outranks a framing violation
        // behind them: past the peer's close_notify nothing is read.
        if (conn_->failed() || conn_->peer_closed()) {
            break; // terminal: the rest drops; the read path surfaces it
        }
        if (framed.fatal) {
            conn_->on_framing_fatal(framed.alert);
            break;
        }
        if (framed.count < batch.size()) {
            // Stopped at an incomplete record (kept for the next read) or
            // the end of the buffer (released: an idle connection pins none).
            if (inbound_.readable() == 0) {
                inbound_ = mem::IoBuf{};
            }
            return;
        }
    }
    inbound_ = mem::IoBuf{};
}

} // namespace fiber::net::detail
