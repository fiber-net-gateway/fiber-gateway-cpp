#include <fiber/net/detail/TlsStreamFd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <sys/uio.h>

#include <fiber/common/Assert.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>
#include <fiber/net/detail/TlsClientStaging.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsTicketService.h>
#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>
#include <fiber/tls/handshake/TlsServerHandshakeEngine.h>
#include <fiber/tls/record/TlsRecord.h>

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

// Wire-read chunk for the engine feeds: large enough that a full flight or
// jumbo app record lands in one try_read, one node's worth of memory.
constexpr std::size_t kReadChunk = 32 * 1024;
constexpr int kMaxIov = 16;
// One TLS record's maximum plaintext — the write-side grouping granularity.
constexpr std::size_t kRecordPlaintextMax = tls::kTlsMaxPlaintextSize;

bool version_bounds_ok(int min_version, int max_version) noexcept {
    const auto in_domain = [](int version) noexcept { return version == 0x0303 || version == 0x0304; };
    return in_domain(min_version) && in_domain(max_version) && min_version <= max_version;
}

} // namespace

// ---------------------------------------------------------------------------
// Handshake staging (09 §5).
//
// A coroutine-frame local of handshake_impl — no TlsStreamFd member holds
// it: the staging and its engines die with the handshake's frame (at
// co_return, or the Canceled unwind of a mid-handshake close), which runs on
// the connection's loop as the engines' node-pool affinity requires.
//
// The staged configs borrow the caller's param material (TlsCredential, ALPN
// backing storage, trust store) under the documented param contract: valid
// until the handshake co_returns. select_server_config re-stages per
// ClientHello through the param's configure callback and latches its error
// for handshake_once to report after flushing the fatal alert.
// ---------------------------------------------------------------------------

struct TlsStreamFd::Handshake {
    const TlsServerParam *param = nullptr; // server only
    tls::TlsClientConfig client_cfg{};
    tls::TlsServerConfig server_cfg{};
    // Ticket service adapters (09 §6): filled from param.ticket_service, the
    // borrowed fn+ctx pairs die with this staging alongside the engine.
    tls::TlsTicketMinter minter{};
    tls::TlsResumptionLookup lookup{};
    common::IoErr callback_error = common::IoErr::None;
    std::array<std::uint8_t, 16> ip_bytes{}; // client verify_ip backing
    tls::TlsClientHandshakeEngine *client = nullptr;
    tls::TlsServerHandshakeEngine *server = nullptr;
    tls::TlsServerConfigSource selector{};

    ~Handshake() {
        delete client;
        delete server;
    }
};

TlsStreamFd::TlsStreamFd(fiber::event::EventLoop &loop, int fd) : stream_fd_(loop, fd) {}

TlsStreamFd::~TlsStreamFd() {
    if (!stream_fd_.valid() && conn_ == nullptr) {
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
    if (conn_ == nullptr) {
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
    if (conn_ == nullptr) {
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
    if (conn_ != nullptr) {
        if (!conn_->failed() && !conn_->peer_closed()) {
            (void) conn_->close_notify();
        }
        fiber::event::IoEvent event = fiber::event::IoEvent::None;
        (void) flush_output(nullptr, event); // best-effort single drain; fd may be gone
        delete conn_;
        conn_ = nullptr;
    }
    // A mid-handshake close touches no handshake state here: the fd close
    // below wakes the suspended handshake with Canceled (inline resume), it
    // unwinds, and its frame-local staging — engines included — dies with the
    // coroutine frame on this loop.
    if (stream_fd_.valid()) {
        stream_fd_.close();
    }
    record_reader_.reset();
    role_ = Role::None;
    handshake_done_ = false;
    shutdown_started_ = false;
    busy_ = false;
    pending_write_chain_ = nullptr;
    pending_write_len_ = 0;
    write_scratch_.reset();
}

fiber::common::IoErr TlsStreamFd::detach_for_handover() noexcept {
    FIBER_ASSERT(!busy_);
    FIBER_ASSERT(pending_write_chain_ == nullptr); // no active write group awaiting retry
    return stream_fd_.detach_for_handover();
}

fiber::common::IoErr TlsStreamFd::adopt_loop(fiber::event::EventLoop &loop) noexcept {
    return stream_fd_.adopt_loop(loop);
}

fiber::common::IoErr TlsStreamFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.set_read_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.set_write_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::set_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.set_terminal_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_read_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_write_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_terminal_callback(callback, ctx);
}

common::IoResult<void> TlsStreamFd::start_client(Handshake &staging, const TlsClientParam &param) noexcept {
    if (role_ != Role::None) {
        return std::unexpected(common::IoErr::Already);
    }
    if (!stream_fd_.valid()) {
        return std::unexpected(common::IoErr::BadFd);
    }
    if (!version_bounds_ok(param.min_version, param.max_version)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto &cfg = staging.client_cfg;
    if (auto staged = TlsClientStager::stage(param, cfg, staging.ip_bytes); !staged) {
        return std::unexpected(staged.error());
    }
    cfg.min_version = static_cast<std::uint16_t>(param.min_version);
    cfg.max_version = static_cast<std::uint16_t>(param.max_version);

    staging.client = new (std::nothrow) tls::TlsClientHandshakeEngine(cfg, nullptr);
    if (staging.client == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    // A construction failure (entropy/allocation) is already terminal with
    // any alert encoded — the handshake loop flushes it, then reports.

    role_ = Role::Client;
    handshake_done_ = false;
    return {};
}

common::IoResult<void> TlsStreamFd::start_server(Handshake &staging, const TlsServerParam &param) noexcept {
    if (role_ != Role::None) {
        return std::unexpected(common::IoErr::Already);
    }
    if (!stream_fd_.valid()) {
        return std::unexpected(common::IoErr::BadFd);
    }
    if (!param.enabled() || !version_bounds_ok(param.min_version, param.max_version)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    // Client-certificate verification needs a trust store up front.
    if (param.client_certificate_mode != TlsClientCertificateMode::None && param.trust_store == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto &cfg = staging.server_cfg;
    if (param.trust_store != nullptr) {
        cfg.client_trust = &param.trust_store->tls_store();
    }
    cfg.require_client_cert = param.client_certificate_mode == TlsClientCertificateMode::Required;
    cfg.alpn = param.alpn;
    cfg.min_version = static_cast<std::uint16_t>(param.min_version);
    cfg.max_version = static_cast<std::uint16_t>(param.max_version);
    cfg.enable_early_data = param.enable_early_data;
    cfg.now_unix_ms = system_now_unix_ms();

    staging.param = &param;
    staging.selector.select = &TlsStreamFd::select_server_config;
    staging.selector.ctx = &staging;
    // Session tickets (09 §6): the service's adapters are staged here so the
    // engine borrows them for the handshake. Unconfigured stays null — no NST,
    // no resumption (decision 1) — which the engine treats natively.
    if (param.ticket_service != nullptr) {
        staging.minter = param.ticket_service->minter();
        staging.lookup = param.ticket_service->lookup();
    }
    // Credentials arrive per ClientHello through the selector, so the
    // template config passes only the selector-mode invariant checks.
    staging.server = new (std::nothrow) tls::TlsServerHandshakeEngine(
            cfg, param.ticket_service != nullptr ? &staging.lookup : nullptr,
            param.ticket_service != nullptr ? &staging.minter : nullptr, &staging.selector);
    if (staging.server == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    role_ = Role::Server;
    handshake_done_ = false;
    return {};
}

const tls::TlsServerConfig *TlsStreamFd::select_server_config(void *ctx,
                                                              const tls::TlsClientHello &client_hello) noexcept {
    auto *staging = static_cast<Handshake *>(ctx);
    staging->callback_error = common::IoErr::None;
    staging->server_cfg.chain = nullptr;
    staging->server_cfg.key = nullptr;
    std::size_t credential_count = 0;
    TlsServerHandshakeConfig config(staging->server_cfg, credential_count);
    const tls::TlsClientHelloView view = client_hello.view();
    common::IoErr error = staging->param->configure_callback(staging->param->configure_ctx, config, view);
    if (error == common::IoErr::None && credential_count == 0) {
        error = common::IoErr::Invalid;
    }
    if (error != common::IoErr::None) {
        staging->callback_error = error;
        return nullptr; // the engine answers handshake_failure
    }
    return &staging->server_cfg;
}

TlsStreamFd::HandshakeTask TlsStreamFd::handshake(const TlsClientParam &param, std::chrono::milliseconds timeout) {
    return handshake_impl(Role::Client, &param, nullptr, timeout);
}

TlsStreamFd::HandshakeTask TlsStreamFd::handshake(const TlsServerParam &param, std::chrono::milliseconds timeout) {
    return handshake_impl(Role::Server, nullptr, &param, timeout);
}

TlsStreamFd::HandshakeTask TlsStreamFd::handshake_impl(Role role, const TlsClientParam *client_param,
                                                       const TlsServerParam *server_param,
                                                       std::chrono::milliseconds timeout) {
    if (busy_) {
        co_return std::unexpected(fiber::common::IoErr::Busy);
    }

    busy_ = true;
    BusyResetGuard busy_reset(&busy_);

    // Frame-local staging: constructed and started only once the coroutine
    // runs, torn down with the frame at co_return or unwind — the engines
    // never outlive the handshake that owns them.
    Handshake staging{};
    common::IoResult<void> start =
            role == Role::Client ? start_client(staging, *client_param) : start_server(staging, *server_param);
    if (!start) {
        co_return std::unexpected(start.error());
    }

    Deadline deadline = make_deadline(timeout);
    for (;;) {
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = handshake_once(staging, wait_event);
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
        if (wait_event == fiber::event::IoEvent::Read) {
            auto wait_result = co_await stream_fd_.wait_readable(*remaining);
            if (!wait_result) {
                co_return std::unexpected(wait_result.error());
            }
            continue;
        }
        if (wait_event == fiber::event::IoEvent::Write) {
            auto wait_result = co_await stream_fd_.wait_writable(*remaining);
            if (!wait_result) {
                co_return std::unexpected(wait_result.error());
            }
            continue;
        }
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
}

StreamFd::WaitReadableAwaiter TlsStreamFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return stream_fd_.wait_readable(timeout);
}

StreamFd::WaitWritableAwaiter TlsStreamFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return stream_fd_.wait_writable(timeout);
}

fiber::common::IoErr TlsStreamFd::poll_shutdown(fiber::event::IoEvent &event) noexcept { return shutdown_once(event); }

fiber::common::IoResult<std::size_t> TlsStreamFd::try_read(std::size_t size, mem::IoBufChain &out) noexcept {
    if (!stream_fd_.valid() || conn_ == nullptr) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (size == 0) {
        return std::size_t{0};
    }
    // A read delivers at most a record's worth of fresh plaintext per call;
    // the take moves record nodes zero-copy, so nothing is allocated unless
    // plaintext actually arrives.
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
        // No plaintext buffered: pull a wire chunk and split its records.
        mem::IoBuf chunk = mem::IoBuf::allocate(kReadChunk);
        if (!chunk.valid()) {
            return std::unexpected(fiber::common::IoErr::NoMem);
        }
        auto read_result = stream_fd_.try_read(chunk.writable_data(), chunk.writable());
        if (!read_result) {
            return std::unexpected(read_result.error()); // WouldBlock included: the socket read is the only stall
        }
        if (*read_result == 0) {
            // EOF without close_notify: truncation.
            return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        chunk.commit(*read_result);
        if (!record_reader_.feed(std::move(chunk))) {
            return std::unexpected(fiber::common::IoErr::NoMem);
        }
        const fiber::common::IoErr drain_err = drain_records();
        if (drain_err != fiber::common::IoErr::None) {
            return std::unexpected(drain_err);
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

fiber::common::IoErr TlsStreamFd::handshake_once(Handshake &staging, fiber::event::IoEvent &event) noexcept {
    if (!stream_fd_.valid()) {
        return fiber::common::IoErr::BadFd;
    }
    if (handshake_done_) {
        if (conn_ != nullptr && conn_->failed()) {
            return fiber::common::IoErr::Invalid;
        }
        return fiber::common::IoErr::None;
    }
    for (;;) {
        // Flights and alerts go out before anything else — including the
        // fatal alert that ends a failed handshake (the failure return only
        // happens once the wire is clean).
        fiber::common::IoErr err = flush_output(&staging, event);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        // The success path falls through to this check on its second pass:
        // tail output flushed, then done (a leftover-fed record violation
        // surfacing as conn_->failed() reports Invalid here).
        if (handshake_done_) {
            if (conn_ != nullptr && conn_->failed()) {
                return fiber::common::IoErr::Invalid;
            }
            return fiber::common::IoErr::None;
        }

        const bool done = role_ == Role::Client ? staging.client->done() : staging.server->done();
        if (!done) {
            err = feed_engine(staging, event);
            if (err != fiber::common::IoErr::None) {
                return err;
            }
            continue;
        }
        if (role_ == Role::Client ? staging.client->failed() : staging.server->failed()) {
            // The alert (if any) is on the wire per the flush above; report
            // the callback's error when one was latched, else the failure.
            // The staging dies with the coroutine frame on the way out.
            const common::IoErr callback_error = staging.callback_error;
            return callback_error != common::IoErr::None ? callback_error : fiber::common::IoErr::Invalid;
        }

        // Success: swap the handshake engine for the connected-phase engine.
        // The engine's pending output (a TLS 1.3 client's Finished record is
        // sealed after this feed and still lives in the engine's chain) must
        // move into the glue's flush chain before the engine dies.
        FIBER_ASSERT(out_pending_.append_chain(role_ == Role::Client ? staging.client->take_output()
                                                                     : staging.server->take_output()));
        tls::TlsConnectedState state =
                role_ == Role::Client ? staging.client->take_state() : staging.server->take_state();
        mem::IoBufChain leftover = role_ == Role::Client ? staging.client->take_inbound_leftover()
                                                         : staging.server->take_inbound_leftover();
        if (role_ == Role::Server) {
            early_data_ = staging.server->take_early_data();
        }
        conn_ = new (std::nothrow) tls::TlsConnection(role_ == Role::Client ? tls::TlsConnectionRole::Client
                                                                            : tls::TlsConnectionRole::Server,
                                                      std::move(state));
        if (conn_ == nullptr) {
            return fiber::common::IoErr::NoMem;
        }
        // The leftover bytes ride the glue's connected-phase reader — records
        // past the final flight (app data piggybacked behind it) frame here.
        if (!leftover.empty() && !record_reader_.feed(std::move(leftover))) {
            return fiber::common::IoErr::NoMem;
        }
        const fiber::common::IoErr drain_err = drain_records();
        if (drain_err != fiber::common::IoErr::None) {
            return drain_err;
        }
        handshake_done_ = true;
    }
}

fiber::common::IoErr TlsStreamFd::shutdown_once(fiber::event::IoEvent &event) noexcept {
    if (!stream_fd_.valid()) {
        return fiber::common::IoErr::BadFd;
    }
    if (!handshake_done_ || conn_ == nullptr) {
        // Nothing to close gracefully (never started / mid-handshake).
        return fiber::common::IoErr::None;
    }
    if (!shutdown_started_) {
        auto closed = conn_->close_notify();
        if (!closed) {
            return closed.error();
        }
        shutdown_started_ = true;
    }
    // Send our close_notify and be done: waiting for the peer's echo is the
    // reader's business (read_once surfaces PeerClosed), not the closer's.
    return flush_output(nullptr, event);
}

// TLS writes seal a contiguous input buffer into records. Unlike sendmsg,
// there is no scatter-gather seal API, so multi-node chains would otherwise
// produce one record per IoBuf: an HTTP/2 DATA frame is a 9-byte header node
// followed by its payload node, and writing the header as its own record
// costs a full send() for 31 bytes on the wire. A node that is itself at
// least one full record is passed to the seal with the node's own pointer
// (zero copy). Anything smaller is coalesced into a scratch buffer with the
// nodes that follow it, splitting the last node when needed, so that each
// scratch write is a full record whenever the chain holds enough data.
fiber::common::IoResult<std::size_t> TlsStreamFd::try_write(mem::IoBufChain &buf) noexcept {
    if (!stream_fd_.valid() || conn_ == nullptr) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (buf.readable_bytes() == 0 && pending_write_chain_ == nullptr) {
        return std::size_t{0};
    }
    if (!out_pending_.empty() && pending_write_chain_ != &buf) {
        // A sealed group is still flushing: only its own chain may resume it
        // (the BoringSSL WANT_WRITE same-buffer contract, chain-shaped).
        return std::unexpected(fiber::common::IoErr::Busy);
    }

    std::size_t group_len = 0;
    if (out_pending_.empty()) {
        std::array<iovec, kMaxIov> iov{};
        const int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        FIBER_ASSERT(count > 0); // readable_bytes() > 0 with no pending group

        const void *write_data = nullptr;
        std::size_t write_len = 0;
        if (count == 1) {
            write_data = iov[0].iov_base;
            write_len = iov[0].iov_len;
        } else if (iov[0].iov_len >= kRecordPlaintextMax) {
            // Whole records straight from the node; its tail joins the next
            // group so it does not become a short record of its own.
            write_data = iov[0].iov_base;
            write_len = iov[0].iov_len - iov[0].iov_len % kRecordPlaintextMax;
        } else {
            if (!write_scratch_) {
                write_scratch_.reset(new (std::nothrow) std::uint8_t[kRecordPlaintextMax]);
                if (!write_scratch_) {
                    return std::unexpected(fiber::common::IoErr::NoMem);
                }
            }
            std::uint8_t *dst = write_scratch_.get();
            std::size_t coalesced = 0;
            for (int i = 0; i < count && coalesced < kRecordPlaintextMax; ++i) {
                const std::size_t take = std::min(iov[i].iov_len, kRecordPlaintextMax - coalesced);
                std::memcpy(dst + coalesced, iov[i].iov_base, take);
                coalesced += take;
            }
            write_data = write_scratch_.get();
            write_len = coalesced;
        }

        group_len = write_len;
        pending_write_chain_ = &buf;
        pending_write_len_ = group_len;
        auto sealed = conn_->write({static_cast<const std::uint8_t *>(write_data), group_len});
        if (!sealed) {
            // The group never sealed: no retry state to keep.
            pending_write_chain_ = nullptr;
            pending_write_len_ = 0;
            return std::unexpected(sealed.error()); // terminal/closed (Invalid) or NoMem
        }
    } else {
        group_len = pending_write_len_; // resume: the plaintext is already sealed
    }

    fiber::event::IoEvent event = fiber::event::IoEvent::None;
    const fiber::common::IoErr err = flush_output(nullptr, event);
    if (err != fiber::common::IoErr::None) {
        if (err != fiber::common::IoErr::WouldBlock) {
            // The group is dead (no resume after a hard error): drop its
            // identity; the sealed remainder lingers until close, and a
            // write on any chain reports Busy meanwhile.
            abandon_pending_write();
        }
        return std::unexpected(err);
    }
    buf.consume_and_compact(group_len);
    pending_write_chain_ = nullptr;
    pending_write_len_ = 0;
    return group_len;
}

void TlsStreamFd::abandon_pending_write() noexcept {
    pending_write_chain_ = nullptr;
    pending_write_len_ = 0;
}

fiber::async::Task<fiber::common::IoResult<std::size_t>> TlsStreamFd::writev(mem::IoBufChain &buf,
                                                                             std::chrono::milliseconds timeout) {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        if (buf.readable_bytes() == 0) {
            co_return std::size_t{0};
        }
        auto written = try_write(buf);
        if (written) {
            // One record group per call — the caller drives the loop.
            co_return written;
        }
        if (written.error() != fiber::common::IoErr::WouldBlock) {
            co_return std::unexpected(written.error());
        }
        // A connected-phase write only ever blocks on writability.
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await stream_fd_.wait_writable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::common::IoErr TlsStreamFd::flush_output(Handshake *staging, fiber::event::IoEvent &event) noexcept {
    if (conn_ != nullptr) {
        FIBER_ASSERT(out_pending_.append_chain(conn_->take_output()));
    } else if (staging != nullptr) {
        FIBER_ASSERT(out_pending_.append_chain(role_ == Role::Client ? staging->client->take_output()
                                                                     : staging->server->take_output()));
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

fiber::common::IoErr TlsStreamFd::feed_engine(Handshake &staging, fiber::event::IoEvent &event) noexcept {
    mem::IoBuf chunk = mem::IoBuf::allocate(kReadChunk);
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
    if (role_ == Role::Client) {
        auto fed = staging.client->feed(std::move(chunk));
        if (!fed) {
            return fed.error();
        }
    } else {
        auto fed = staging.server->feed(std::move(chunk));
        if (!fed) {
            return fed.error();
        }
    }
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::drain_records() noexcept {
    FIBER_ASSERT(conn_ != nullptr);
    for (;;) {
        tls::TlsRecordReader::Result next = record_reader_.next();
        if (next.status == tls::TlsRecordReader::Result::Status::Fatal) {
            conn_->on_framing_fatal(next.alert);
            return fiber::common::IoErr::None; // the read path surfaces the latched terminal
        }
        if (next.status == tls::TlsRecordReader::Result::Status::NeedMore) {
            return fiber::common::IoErr::None; // partial record tail stays buffered here
        }
        conn_->on_record(std::move(next.record));
        if (conn_->failed() || conn_->peer_closed()) {
            return fiber::common::IoErr::None; // terminal: further records drop
        }
    }
}

} // namespace fiber::net::detail
