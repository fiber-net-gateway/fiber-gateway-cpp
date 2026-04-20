#include "Http3Engine.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>

#include <netinet/in.h>

#include <lsquic.h>
#include <lsxpack_header.h>

#include "../event/EventLoop.h"
#include "../net/UdpPacket.h"
#include "../net/UdpSocket.h"
#include "TlsContext.h"

namespace fiber::http {

namespace {

using fiber::common::IoErr;
using fiber::event::IoEvent;

constexpr std::size_t kMaxDatagram = 65535;
constexpr std::size_t kHeaderDecodeBufferSize = 8192;
constexpr std::string_view kHttp3Alpn = "h3";
constexpr std::chrono::milliseconds kBlockedTxPoll{5};

unsigned g_lsquic_global_ref_count = 0;
unsigned g_lsquic_global_flags = 0;

common::IoResult<void> acquire_lsquic_global(unsigned flags) noexcept {
    if (g_lsquic_global_ref_count == 0) {
        if (lsquic_global_init(flags) != 0) {
            return std::unexpected(IoErr::Invalid);
        }
        g_lsquic_global_flags = flags;
    } else if (g_lsquic_global_flags != flags) {
        return std::unexpected(IoErr::Invalid);
    }
    ++g_lsquic_global_ref_count;
    return {};
}

void release_lsquic_global() noexcept {
    if (g_lsquic_global_ref_count == 0) {
        return;
    }
    --g_lsquic_global_ref_count;
    if (g_lsquic_global_ref_count == 0) {
        g_lsquic_global_flags = 0;
        lsquic_global_cleanup();
    }
}

bool is_v4_mapped_ipv6(const std::array<std::uint8_t, 16> &bytes) noexcept {
    for (std::size_t i = 0; i < 10; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return bytes[10] == 0xFF && bytes[11] == 0xFF;
}

net::SocketAddress normalize_socket_address(const net::SocketAddress &addr) noexcept {
    if (!addr.ip().is_v6()) {
        return addr;
    }
    const auto &bytes = addr.ip().v6_bytes();
    if (!is_v4_mapped_ipv6(bytes)) {
        return addr;
    }
    std::array<std::uint8_t, 4> v4{};
    std::memcpy(v4.data(), bytes.data() + 12, v4.size());
    return {net::IpAddress::v4(v4), addr.port()};
}

net::UdpEcn to_udp_ecn(int ecn) noexcept {
    switch (ecn & 0x03) {
        case 0:
            return net::UdpEcn::NonEct;
        case 1:
            return net::UdpEcn::Ect1;
        case 2:
            return net::UdpEcn::Ect0;
        case 3:
            return net::UdpEcn::Ce;
        default:
            return net::UdpEcn::Unspecified;
    }
}

int to_lsquic_ecn(net::UdpEcn ecn) noexcept {
    if (ecn == net::UdpEcn::Unspecified) {
        return 0;
    }
    return static_cast<int>(ecn) & 0x03;
}

bool to_socket_address(const sockaddr *sa, net::SocketAddress &out) noexcept {
    if (!sa) {
        return false;
    }
    socklen_t len = 0;
    switch (sa->sa_family) {
        case AF_INET:
            len = sizeof(sockaddr_in);
            break;
        case AF_INET6:
            len = sizeof(sockaddr_in6);
            break;
        default:
            return false;
    }
    net::SocketAddress parsed;
    if (!net::SocketAddress::from_sockaddr(sa, len, parsed)) {
        return false;
    }
    out = normalize_socket_address(parsed);
    return true;
}

bool to_sockaddr(const net::SocketAddress &addr, sockaddr_storage &storage, socklen_t &len) noexcept {
    return addr.to_sockaddr(storage, len);
}

IoErr io_err_from_stream_errno() noexcept {
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK) {
        return IoErr::WouldBlock;
    }
    return common::io_err_from_errno(err);
}

struct HeaderSet {
    lsxpack_header header{};
    std::array<char, kHeaderDecodeBufferSize> decode_buf{};
    std::size_t decode_off = 0;
    bool have_header = false;
};

void *create_header_set(void *, lsquic_stream_t *, int) { return new (std::nothrow) HeaderSet(); }

lsxpack_header *prepare_decode_header(void *header_set, lsxpack_header *hdr, std::size_t space) {
    auto *headers = static_cast<HeaderSet *>(header_set);
    if (!headers) {
        return nullptr;
    }
    if (headers->have_header) {
        headers->decode_off += lsxpack_header_get_dec_size(&headers->header);
    } else {
        headers->have_header = true;
    }
    if (headers->decode_off + space > headers->decode_buf.size()) {
        return nullptr;
    }
    lsxpack_header *target = hdr ? hdr : &headers->header;
    lsxpack_header_prepare_decode(target, headers->decode_buf.data(), headers->decode_off,
                                  headers->decode_buf.size() - headers->decode_off);
    return target;
}

int process_decoded_header(void *, lsxpack_header *) { return 0; }

void discard_header_set(void *header_set) { delete static_cast<HeaderSet *>(header_set); }

const lsquic_hset_if kHeaderSetIf{
        .hsi_create_header_set = create_header_set,
        .hsi_prepare_decode = prepare_decode_header,
        .hsi_process_header = process_decoded_header,
        .hsi_discard_header_set = discard_header_set,
        .hsi_flags = static_cast<lsquic_hsi_flag>(0),
};

} // namespace

class Http3Engine::Impl {
public:
    Impl(event::EventLoop &loop, net::UdpSocket &udp, Http3EngineConfig config) noexcept :
        loop_(&loop), udp_(&udp), config_(std::move(config)) {}

    ~Impl() { close(); }

    common::IoResult<void> init() noexcept;
    async::DetachedTask run() noexcept;
    void stop() noexcept { stopping_ = true; }
    void close() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool running() const noexcept { return running_; }

private:
    struct ConnCtx {
        Impl *engine = nullptr;
        Http3Connection conn{};
    };

    struct StreamCtx {
        Impl *engine = nullptr;
        ConnCtx *conn = nullptr;
        Http3Stream stream{};
    };

    static int packets_out_cb(void *ctx, const lsquic_out_spec *specs, unsigned n_specs);
    static SSL_CTX *get_ssl_ctx_cb(void *peer_ctx, const sockaddr *local);
    static SSL_CTX *lookup_cert_cb(void *ctx, const sockaddr *local, const char *sni);
    static lsquic_conn_ctx_t *on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn);
    static void on_conn_closed(lsquic_conn_t *conn);
    static lsquic_stream_ctx_t *on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream);
    static void on_read(lsquic_stream_t *, lsquic_stream_ctx_t *h);
    static void on_write(lsquic_stream_t *, lsquic_stream_ctx_t *h);
    static void on_close(lsquic_stream_t *, lsquic_stream_ctx_t *h);

    [[nodiscard]] static const lsquic_stream_if &stream_if() noexcept;

    [[nodiscard]] common::IoResult<SSL_CTX *> select_ssl_ctx(std::string_view server_name,
                                                             const sockaddr *local) noexcept;
    [[nodiscard]] bool fill_send_spec(const lsquic_out_spec &src, net::UdpPacketSendSpec &dst) noexcept;
    void drain_udp() noexcept;
    void process_conns() noexcept;
    void send_unsent_packets() noexcept;
    [[nodiscard]] int compute_wait_timeout_ms() const noexcept;

    event::EventLoop *loop_ = nullptr;
    net::UdpSocket *udp_ = nullptr;
    Http3EngineConfig config_{};
    lsquic_engine_t *engine_ = nullptr;
    lsquic_engine_settings settings_{};
    bool stopping_ = false;
    bool tx_blocked_ = false;
    bool initialized_ = false;
    bool running_ = false;
    bool global_acquired_ = false;
    std::uint64_t packets_in_ = 0;
    std::uint64_t packets_out_ = 0;
    std::uint64_t send_blocked_ = 0;
    std::uint64_t packet_in_errors_ = 0;
    std::uint64_t hard_send_errors_ = 0;
};

Http3Connection::Http3Connection(void *native, net::SocketAddress local_addr, net::SocketAddress remote_addr) noexcept :
    native_(native), local_addr_(std::move(local_addr)), remote_addr_(std::move(remote_addr)) {}

void Http3Connection::close() noexcept {
    if (!native_) {
        return;
    }
    lsquic_conn_close(static_cast<lsquic_conn_t *>(native_));
}

Http3Stream::Http3Stream(void *native, Http3Connection *conn) noexcept : native_(native), conn_(conn) {}

common::IoResult<void> Http3Stream::want_read(bool enabled) noexcept {
    if (!native_) {
        return std::unexpected(IoErr::Invalid);
    }
    lsquic_stream_wantread(static_cast<lsquic_stream_t *>(native_), enabled ? 1 : 0);
    return {};
}

common::IoResult<void> Http3Stream::want_write(bool enabled) noexcept {
    if (!native_) {
        return std::unexpected(IoErr::Invalid);
    }
    lsquic_stream_wantwrite(static_cast<lsquic_stream_t *>(native_), enabled ? 1 : 0);
    return {};
}

common::IoResult<std::size_t> Http3Stream::read(void *buf, std::size_t len) noexcept {
    if (!native_ || (!buf && len != 0)) {
        return std::unexpected(IoErr::Invalid);
    }
    ssize_t rc = lsquic_stream_read(static_cast<lsquic_stream_t *>(native_), buf, len);
    if (rc < 0) {
        return std::unexpected(io_err_from_stream_errno());
    }
    return static_cast<std::size_t>(rc);
}

common::IoResult<std::size_t> Http3Stream::write(const void *buf, std::size_t len) noexcept {
    if (!native_ || (!buf && len != 0)) {
        return std::unexpected(IoErr::Invalid);
    }
    ssize_t rc = lsquic_stream_write(static_cast<lsquic_stream_t *>(native_), buf, len);
    if (rc < 0) {
        return std::unexpected(io_err_from_stream_errno());
    }
    return static_cast<std::size_t>(rc);
}

common::IoResult<void> Http3Stream::shutdown_write() noexcept {
    if (!native_) {
        return std::unexpected(IoErr::Invalid);
    }
    if (lsquic_stream_shutdown(static_cast<lsquic_stream_t *>(native_), 1) != 0) {
        return std::unexpected(io_err_from_stream_errno());
    }
    return {};
}

void Http3Stream::close() noexcept {
    if (!native_) {
        return;
    }
    lsquic_stream_close(static_cast<lsquic_stream_t *>(native_));
}

common::IoResult<void> Http3Engine::Impl::init() noexcept {
    if (initialized_) {
        return {};
    }
    if (!loop_ || !udp_ || !udp_->valid() || !config_.identity_selector.select || !config_.handler.ops) {
        return std::unexpected(IoErr::Invalid);
    }

    auto global_result = acquire_lsquic_global(LSQUIC_GLOBAL_SERVER);
    if (!global_result) {
        return std::unexpected(global_result.error());
    }
    global_acquired_ = true;

    const unsigned flags = LSENG_SERVER | LSENG_HTTP;
    lsquic_engine_init_settings(&settings_, flags);
    settings_.es_support_srej = 0;
    settings_.es_idle_timeout = static_cast<unsigned>(config_.options.idle_timeout.count());
    settings_.es_init_max_streams_bidi = config_.options.max_bidirectional_streams;

    lsquic_engine_api api{};
    api.ea_settings = &settings_;
    api.ea_stream_if = &stream_if();
    api.ea_stream_if_ctx = this;
    api.ea_packets_out = &Impl::packets_out_cb;
    api.ea_packets_out_ctx = this;
    api.ea_get_ssl_ctx = &Impl::get_ssl_ctx_cb;
    api.ea_lookup_cert = &Impl::lookup_cert_cb;
    api.ea_cert_lu_ctx = this;
    api.ea_hsi_if = &kHeaderSetIf;

    engine_ = lsquic_engine_new(flags, &api);
    if (!engine_) {
        release_lsquic_global();
        global_acquired_ = false;
        return std::unexpected(IoErr::Invalid);
    }

    initialized_ = true;
    return {};
}

async::DetachedTask Http3Engine::Impl::run() noexcept {
    if (!initialized_ || !engine_ || !udp_) {
        co_return;
    }

    running_ = true;
    while (!stopping_) {
        IoEvent interested = IoEvent::Read;
        if (tx_blocked_) {
            interested |= IoEvent::Write;
        }

        auto wait_result = co_await udp_->wait_event(interested, std::chrono::milliseconds(compute_wait_timeout_ms()));
        if (wait_result) {
            if (event::any(*wait_result & IoEvent::Read)) {
                drain_udp();
            }
            if (event::any(*wait_result & IoEvent::Write)) {
                tx_blocked_ = false;
                send_unsent_packets();
            }
        } else if (wait_result.error() != IoErr::TimedOut && wait_result.error() != IoErr::Interrupted) {
            break;
        }

        process_conns();
        if (lsquic_engine_has_unsent_packets(engine_)) {
            send_unsent_packets();
            if (lsquic_engine_has_unsent_packets(engine_)) {
                tx_blocked_ = true;
            }
        }
    }
    running_ = false;
    co_return;
}

void Http3Engine::Impl::close() noexcept {
    stopping_ = true;
    running_ = false;
    if (engine_) {
        lsquic_engine_destroy(engine_);
        engine_ = nullptr;
    }
    initialized_ = false;
    if (global_acquired_) {
        release_lsquic_global();
        global_acquired_ = false;
    }
}

int Http3Engine::Impl::packets_out_cb(void *ctx, const lsquic_out_spec *specs, unsigned n_specs) {
    auto *self = static_cast<Impl *>(ctx);
    if (!self || !self->udp_) {
        return -1;
    }

    unsigned sent = 0;
    for (unsigned i = 0; i < n_specs; ++i) {
        net::UdpPacketSendSpec spec{};
        if (!self->fill_send_spec(specs[i], spec)) {
            break;
        }

        auto send_result = self->udp_->try_send_packet(spec);
        if (send_result) {
            ++sent;
            ++self->packets_out_;
            continue;
        }
        if (send_result.error() == IoErr::MessageTooLarge) {
            ++self->hard_send_errors_;
            continue;
        }
        if (send_result.error() == IoErr::WouldBlock) {
            self->tx_blocked_ = true;
            ++self->send_blocked_;
            break;
        }
        ++self->hard_send_errors_;
        break;
    }
    return static_cast<int>(sent);
}

SSL_CTX *Http3Engine::Impl::get_ssl_ctx_cb(void *peer_ctx, const sockaddr *local) {
    auto *self = static_cast<Impl *>(peer_ctx);
    if (!self) {
        return nullptr;
    }
    auto result = self->select_ssl_ctx({}, local);
    return result ? *result : nullptr;
}

SSL_CTX *Http3Engine::Impl::lookup_cert_cb(void *ctx, const sockaddr *local, const char *sni) {
    auto *self = static_cast<Impl *>(ctx);
    if (!self) {
        return nullptr;
    }
    auto result = self->select_ssl_ctx(sni ? std::string_view(sni) : std::string_view{}, local);
    return result ? *result : nullptr;
}

lsquic_conn_ctx_t *Http3Engine::Impl::on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    auto *self = static_cast<Impl *>(stream_if_ctx);
    if (!self || !conn) {
        return nullptr;
    }

    const sockaddr *local_sa = nullptr;
    const sockaddr *peer_sa = nullptr;
    net::SocketAddress local;
    net::SocketAddress peer;
    if (lsquic_conn_get_sockaddr(conn, &local_sa, &peer_sa) == 0) {
        (void) to_socket_address(local_sa, local);
        (void) to_socket_address(peer_sa, peer);
    }

    auto *ctx = new (std::nothrow) ConnCtx();
    if (!ctx) {
        return nullptr;
    }
    ctx->engine = self;
    ctx->conn = Http3EngineAccess::make_connection(conn, local, peer);

    const auto *ops = self->config_.handler.ops;
    if (ops && ops->on_connection_opened) {
        ops->on_connection_opened(self->config_.handler.ctx, ctx->conn);
    }
    return reinterpret_cast<lsquic_conn_ctx_t *>(ctx);
}

void Http3Engine::Impl::on_conn_closed(lsquic_conn_t *conn) {
    auto *ctx = reinterpret_cast<ConnCtx *>(lsquic_conn_get_ctx(conn));
    if (!ctx) {
        return;
    }

    auto *self = ctx->engine;
    const auto *ops = self ? self->config_.handler.ops : nullptr;
    if (ops && ops->on_connection_closed) {
        ops->on_connection_closed(self->config_.handler.ctx, ctx->conn);
    }
    Http3EngineAccess::clear(ctx->conn);
    lsquic_conn_set_ctx(conn, nullptr);
    delete ctx;
}

lsquic_stream_ctx_t *Http3Engine::Impl::on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    auto *self = static_cast<Impl *>(stream_if_ctx);
    if (!self || !stream) {
        return nullptr;
    }

    auto *conn_ctx = reinterpret_cast<ConnCtx *>(lsquic_conn_get_ctx(lsquic_stream_conn(stream)));
    auto *ctx = new (std::nothrow) StreamCtx();
    if (!ctx) {
        return nullptr;
    }
    ctx->engine = self;
    ctx->conn = conn_ctx;
    ctx->stream = Http3EngineAccess::make_stream(stream, conn_ctx ? &conn_ctx->conn : nullptr);

    const auto *ops = self->config_.handler.ops;
    if (ops && ops->on_stream_opened) {
        ops->on_stream_opened(self->config_.handler.ctx, ctx->stream);
    }
    return reinterpret_cast<lsquic_stream_ctx_t *>(ctx);
}

void Http3Engine::Impl::on_read(lsquic_stream_t *, lsquic_stream_ctx_t *h) {
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    if (!ctx || !ctx->engine) {
        return;
    }
    const auto *ops = ctx->engine->config_.handler.ops;
    if (ops && ops->on_stream_readable) {
        ops->on_stream_readable(ctx->engine->config_.handler.ctx, ctx->stream);
    }
}

void Http3Engine::Impl::on_write(lsquic_stream_t *, lsquic_stream_ctx_t *h) {
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    if (!ctx || !ctx->engine) {
        return;
    }
    const auto *ops = ctx->engine->config_.handler.ops;
    if (ops && ops->on_stream_writable) {
        ops->on_stream_writable(ctx->engine->config_.handler.ctx, ctx->stream);
    }
}

void Http3Engine::Impl::on_close(lsquic_stream_t *, lsquic_stream_ctx_t *h) {
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    if (!ctx) {
        return;
    }
    auto *self = ctx->engine;
    const auto *ops = self ? self->config_.handler.ops : nullptr;
    if (ops && ops->on_stream_closed) {
        ops->on_stream_closed(self->config_.handler.ctx, ctx->stream);
    }
    Http3EngineAccess::clear(ctx->stream);
    delete ctx;
}

const lsquic_stream_if &Http3Engine::Impl::stream_if() noexcept {
    static const lsquic_stream_if kStreamIf{
            .on_new_conn = &Impl::on_new_conn,
            .on_goaway_received = nullptr,
            .on_conn_closed = &Impl::on_conn_closed,
            .on_new_stream = &Impl::on_new_stream,
            .on_read = &Impl::on_read,
            .on_write = &Impl::on_write,
            .on_close = &Impl::on_close,
    };
    return kStreamIf;
}

common::IoResult<SSL_CTX *> Http3Engine::Impl::select_ssl_ctx(std::string_view server_name,
                                                              const sockaddr *local) noexcept {
    net::SocketAddress local_addr;
    const net::SocketAddress *local_ptr = nullptr;
    if (local && to_socket_address(local, local_addr)) {
        local_ptr = &local_addr;
    }

    TlsIdentitySelectInput input{
            .server_name = server_name,
            .alpn = {},
            .selected_alpn = kHttp3Alpn,
            .remote_addr = nullptr,
            .local_addr = local_ptr,
            .server_context = nullptr,
            .transport = TlsTransportKind::Quic,
    };

    TlsContext *selected = config_.identity_selector.select(config_.identity_selector.ctx, input);
    if (!selected || !selected->raw()) {
        return std::unexpected(IoErr::Invalid);
    }
    return selected->raw();
}

bool Http3Engine::Impl::fill_send_spec(const lsquic_out_spec &src, net::UdpPacketSendSpec &dst) noexcept {
    if (!to_socket_address(src.dest_sa, dst.peer)) {
        return false;
    }
    dst.iov = src.iov;
    if (src.iovlen > static_cast<unsigned>(std::numeric_limits<int>::max())) {
        return false;
    }
    dst.iov_count = static_cast<int>(src.iovlen);
    dst.ecn = src.ecn >= 0 ? to_udp_ecn(src.ecn) : net::UdpEcn::Unspecified;
    if (!src.local_sa) {
        return true;
    }
    if (!to_socket_address(src.local_sa, dst.local)) {
        return false;
    }
    dst.has_local = true;
    return true;
}

void Http3Engine::Impl::drain_udp() noexcept {
    std::array<unsigned char, kMaxDatagram> packet{};
    for (;;) {
        auto recv_result = udp_->try_recv_packet(packet.data(), packet.size());
        if (!recv_result) {
            if (recv_result.error() == IoErr::WouldBlock) {
                break;
            }
            if (recv_result.error() == IoErr::Interrupted) {
                continue;
            }
            ++packet_in_errors_;
            break;
        }
        if (recv_result->size == 0) {
            continue;
        }

        recv_result->peer = normalize_socket_address(recv_result->peer);
        recv_result->local = normalize_socket_address(recv_result->local);

        sockaddr_storage local{};
        sockaddr_storage peer{};
        socklen_t local_len = 0;
        socklen_t peer_len = 0;
        if (!to_sockaddr(recv_result->local, local, local_len) || !to_sockaddr(recv_result->peer, peer, peer_len)) {
            ++packet_in_errors_;
            continue;
        }

        lsquic_engine_packet_in(engine_, packet.data(), recv_result->size, reinterpret_cast<const sockaddr *>(&local),
                                reinterpret_cast<const sockaddr *>(&peer), this, to_lsquic_ecn(recv_result->ecn));
        ++packets_in_;
    }
}

void Http3Engine::Impl::process_conns() noexcept {
    if (engine_) {
        lsquic_engine_process_conns(engine_);
    }
}

void Http3Engine::Impl::send_unsent_packets() noexcept {
    if (engine_) {
        lsquic_engine_send_unsent_packets(engine_);
    }
}

int Http3Engine::Impl::compute_wait_timeout_ms() const noexcept {
    int timeout_ms = static_cast<int>(config_.options.max_wait.count());
    int diff_us = 0;
    if (engine_ && lsquic_engine_earliest_adv_tick(engine_, &diff_us)) {
        timeout_ms = diff_us <= 0 ? 0 : static_cast<int>((diff_us + 999) / 1000);
    }
    const int max_wait = static_cast<int>(config_.options.max_wait.count());
    if (max_wait >= 0 && timeout_ms > max_wait) {
        timeout_ms = max_wait;
    }
    if (tx_blocked_ && timeout_ms > static_cast<int>(kBlockedTxPoll.count())) {
        timeout_ms = static_cast<int>(kBlockedTxPoll.count());
    }
    return timeout_ms < 0 ? 0 : timeout_ms;
}

Http3Engine::Http3Engine(event::EventLoop &loop, net::UdpSocket &udp, Http3EngineConfig config) noexcept :
    impl_(new(std::nothrow) Impl(loop, udp, std::move(config))) {}

Http3Engine::~Http3Engine() = default;

common::IoResult<void> Http3Engine::init() noexcept {
    if (!impl_) {
        return std::unexpected(IoErr::NoMem);
    }
    return impl_->init();
}

async::DetachedTask Http3Engine::run() noexcept {
    if (!impl_) {
        return {};
    }
    return impl_->run();
}

void Http3Engine::stop() noexcept {
    if (impl_) {
        impl_->stop();
    }
}

void Http3Engine::close() noexcept {
    if (impl_) {
        impl_->close();
    }
}

bool Http3Engine::initialized() const noexcept { return impl_ && impl_->initialized(); }

bool Http3Engine::running() const noexcept { return impl_ && impl_->running(); }

} // namespace fiber::http
