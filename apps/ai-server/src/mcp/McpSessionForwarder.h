#ifndef FIBER_AI_SERVER_MCP_SESSION_FORWARDER_H
#define FIBER_AI_SERVER_MCP_SESSION_FORWARDER_H

#include "../limit/RateLimitShardRing.h"

#include <cstdint>
#include <string_view>

#include <async/Task.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoopGroup.h>
#include <http/LocalHttp1ConnectionPoolSet.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::ai_server {

enum class McpForwardResult : std::uint8_t {
    Completed,
    NodeNotFound,
    Failed,
};

class McpSessionForwarder final : public common::NonCopyable, public common::NonMovable {
public:
    McpSessionForwarder(event::EventLoopGroup &workers, RateLimitShardRing &ring) noexcept;
    ~McpSessionForwarder();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Task<McpForwardResult> forward(http::HttpExchange &exchange,
                                                        std::string_view node_id) noexcept;

private:
    RateLimitShardRing *ring_ = nullptr;
    http::LocalHttp1ConnectionPoolSet pool_;
    bool initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_SESSION_FORWARDER_H
