#ifndef FIBER_AI_SERVER_AI_SERVER_H
#define FIBER_AI_SERVER_AI_SERVER_H

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "common/NonCopyable.h"
#include "common/NonMovable.h"
#include "event/EventLoop.h"
#include "http/Http1Server.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace fiber::ai_server {

class LlmConfigManager;

class AiServer final : public common::NonCopyable, public common::NonMovable {
public:
    AiServer(event::EventLoop &loop, const LlmConfigManager &config_manager);

    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &address, const net::ListenOptions &options);
    async::DetachedTask serve();
    void close();
    [[nodiscard]] async::Task<void> shutdown_and_wait();
    [[nodiscard]] int fd() const noexcept;

private:
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange);

    const LlmConfigManager *config_manager_ = nullptr;
    http::Http1Server server_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_H
