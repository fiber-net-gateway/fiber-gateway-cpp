#include <fiber/http/endpoint/TcpEndpointBase.h>

#include <algorithm>
#include <cerrno>
#include <new>
#include <sys/socket.h>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <fiber/tls/TlsTicketService.h>

namespace fiber::http {

namespace {

common::IoResult<net::SocketAddress> resolve_local_addr(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t len = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &len) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }

    net::SocketAddress local;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), len, local)) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    return local;
}

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

TcpEndpointBase::TcpEndpointBase(net::SocketAddress address, net::ListenOptions listen, net::TcpSocketOptions tcp,
                                 HttpServerTlsOptions tls) noexcept :
    address_(std::move(address)), listen_(listen), tcp_(tcp), tls_(tls) {}

void TcpEndpointWorkerBase::attach(TcpEndpointBase &endpoint, std::size_t index) noexcept {
    endpoint_ = &endpoint;
    index_ = index;
}

TcpEndpointWorkerBase::~TcpEndpointWorkerBase() {
    if (endpoint_) {
        endpoint_->workers_[index_] = nullptr;
    }
}

TcpEndpointBase::~TcpEndpointBase() {
    FIBER_ASSERT(std::all_of(workers_.begin(), workers_.end(), [](auto *worker) { return worker == nullptr; }));
}

common::IoResult<void> TcpEndpointBase::build_ticket_service() noexcept {
    if (tls_.ticket_keys.empty()) {
        return {};
    }
    if (tls_.ticket_keys.size() > tls::TlsTicketService::kMaxKeys) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::array<tls::TlsTicketKeyMaterial, tls::TlsTicketService::kMaxKeys> material{};
    for (std::size_t i = 0; i < tls_.ticket_keys.size(); ++i) {
        const HttpServerTlsTicketKey &configured = tls_.ticket_keys[i];
        const std::string_view hex = configured.key_hex;
        if (hex.size() != material[i].bytes.size() * 2) {
            return std::unexpected(common::IoErr::Invalid);
        }
        for (std::size_t j = 0; j < material[i].bytes.size(); ++j) {
            const int high = hex_digit(hex[2 * j]);
            const int low = hex_digit(hex[2 * j + 1]);
            if (high < 0 || low < 0) {
                return std::unexpected(common::IoErr::Invalid);
            }
            material[i].bytes[j] = static_cast<std::uint8_t>((high << 4) | low);
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (material[j].id == configured.id) {
                return std::unexpected(common::IoErr::Invalid);
            }
        }
        material[i].id = configured.id;
        material[i].created_ms = configured.created_ms;
    }
    // Window policy keeps the library defaults (24 h mint / 7 d retention):
    // no deployment has asked to tune it, and the ticket timeout the mint
    // side advertises (2 h) sits comfortably inside. Only the configured
    // prefix of the staging array rides into the service — the zero-filled
    // tail would arrive as duplicate id-0 keys.
    const tls::TlsTicketKeyPolicy policy{};
    auto service = std::unique_ptr<tls::TlsTicketService>(new (std::nothrow) tls::TlsTicketService(
            std::span<const tls::TlsTicketKeyMaterial>(material.data(), tls_.ticket_keys.size()), policy));
    if (service == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (!service->valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    ticket_service_ = std::move(service);
    return {};
}

common::IoResult<void> TcpEndpointBase::on_start(Server &server) noexcept {
    FIBER_ASSERT(!listener_ || !listener_->valid());
    FIBER_ASSERT(std::all_of(workers_.begin(), workers_.end(), [](auto *worker) { return worker == nullptr; }));
    listener_.reset();
    workers_.clear();
    next_worker_ = 0;
    server_ = &server;
    ticket_service_.reset();

    // Fail on bad ticket material before anything binds.
    auto tickets = build_ticket_service();
    if (!tickets) {
        return std::unexpected(tickets.error());
    }

    auto listener = std::unique_ptr<net::TcpListener>(new (std::nothrow) net::TcpListener(server.owner_loop()));
    if (!listener) {
        return std::unexpected(common::IoErr::NoMem);
    }

    auto bound = listener->bind(address_, listen_);
    if (!bound) {
        return std::unexpected(bound.error());
    }

    // Resolve the kernel-assigned port so a port-0 endpoint can report where it
    // actually landed (and so an HTTP/3 endpoint can inherit it).
    auto local = resolve_local_addr(listener->fd());
    if (!local) {
        listener->close();
        return std::unexpected(local.error());
    }

    local_addr_ = *local;
    listener_ = std::move(listener);
    workers_.reserve(server.worker_count());
    return {};
}

EndpointWorker *TcpEndpointBase::create_worker(event::EventLoop &loop, std::size_t index) noexcept {
    TcpEndpointWorkerBase *worker = make_worker(loop, index);
    if (worker == nullptr) {
        return nullptr;
    }
    if (workers_.size() <= index) {
        workers_.resize(index + 1, nullptr);
    }
    workers_[index] = worker;
    worker->attach(*this, index);
    return worker;
}

async::Task<void> TcpEndpointBase::on_serve() noexcept {
    if (server_->draining() || !listener_ || workers_.empty()) {
        co_return;
    }
    FIBER_ASSERT(listener_->loop().in_loop());

    while (listener_->valid()) {
        auto accept_result = co_await listener_->accept();
        if (!accept_result) {
            const common::IoErr error = accept_result.error();
            if (error == common::IoErr::Canceled || error == common::IoErr::BadFd) {
                break;
            }
            continue;
        }

        // Shutdown ordering: Server::stop() publishes Draining before posting
        // the owner-loop teardown, and only that teardown notifies the workers.
        // This check and the begin_task()/spawn below are one uninterrupted
        // stretch on the owner loop, so a worker that is not draining here
        // cannot already have completed wait_stopped() and been destroyed.
        if (server_->draining()) {
            continue; // AcceptResult closes the fd
        }

        TcpEndpointWorkerBase *worker = workers_[next_worker_ % workers_.size()];
        ++next_worker_;
        worker->begin_task();
        async::spawn(worker->loop(),
                     [this, worker, accept = std::move(*accept_result)]() mutable -> async::DetachedTask {
                         return run_accepted(this, worker, std::move(accept));
                     });
    }
    co_return;
}

async::DetachedTask TcpEndpointBase::run_accepted(TcpEndpointBase *self, TcpEndpointWorkerBase *worker,
                                                  net::AcceptResult accept) noexcept {
    struct TaskGuard {
        TcpEndpointWorkerBase *worker;
        ~TaskGuard() { worker->end_task(); }
    } guard{worker};

    if (worker->draining()) {
        co_return;
    }

    std::unique_ptr<HttpTransport> transport;
    if (self->tls_.enabled()) {
        auto tls_result = TlsTransport::create(worker->loop(), std::move(accept), self->tcp_);
        if (!tls_result) {
            co_return;
        }
        auto tls_transport = std::move(*tls_result);
        auto tls_param = self->make_tls_param();
        auto handshaken = co_await tls_transport->handshake(tls_param, net::kDefaultTlsHandshakeTimeout);
        if (!handshaken) {
            tls_transport->close();
            co_return;
        }
        transport = std::move(tls_transport);
    } else {
        auto tcp_result = TcpTransport::create(worker->loop(), std::move(accept), self->tcp_);
        if (!tcp_result) {
            co_return;
        }
        transport = std::move(*tcp_result);
    }

    // The worker may have started draining while the handshake was in flight.
    if (worker->draining()) {
        transport->close();
        co_return;
    }

    co_await self->serve_connection(*worker, std::move(transport));
    co_return;
}

void TcpEndpointBase::on_stop() noexcept {
    if (listener_) {
        listener_->close();
    }
}

} // namespace fiber::http
