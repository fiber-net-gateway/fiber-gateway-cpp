#ifndef FIBER_AI_SERVER_AI_SERVER_CAT_REQUEST_H
#define FIBER_AI_SERVER_AI_SERVER_CAT_REQUEST_H

#include <cstddef>
#include <expected>
#include <optional>
#include <string_view>

#include <fiber/cat/Event.h>
#include <fiber/cat/MessageTrace.h>
#include <fiber/cat/Transaction.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::cat {
class CatClient;
}

namespace fiber::mem {
class BufPool;
}

namespace fiber::http {
class HttpExchange;
class HttpHeaders;
} // namespace fiber::http

namespace fiber::ai_server {

inline constexpr std::size_t kMaxAiServerTraceStateBytes = 512;
inline constexpr std::size_t kMaxAiServerCatUserAgentBytes = 1024;

[[nodiscard]] cat::MessageTraceContext read_cat_trace_context(const http::HttpHeaders &headers) noexcept;

[[nodiscard]] bool inject_cat_headers(http::HttpHeaders &headers, const cat::MessageTraceContext *context,
                                      std::string_view trace_state = {}) noexcept;

class AiServerCatRequest final : public common::NonCopyable, public common::NonMovable {
public:
    AiServerCatRequest(http::HttpExchange &exchange, cat::CatClient *client) noexcept;
    ~AiServerCatRequest();

    [[nodiscard]] cat::Transaction *root_transaction() noexcept { return root_ && root_->valid() ? &*root_ : nullptr; }
    [[nodiscard]] std::string_view request_id() const noexcept;
    [[nodiscard]] std::string_view trace_state() const noexcept { return trace_state_; }
    void inject_response_header(http::HttpHeaders &headers) const noexcept;
    cat::RecordError add_root_data(std::string_view key, std::string_view value) noexcept;
    cat::RecordError set_root_model_name(std::string_view model) noexcept;

    [[nodiscard]] std::expected<cat::MessageTraceContext, cat::RecordError>
    create_remote_context(cat::Transaction *parent = nullptr) noexcept;
    [[nodiscard]] std::expected<cat::MessageTraceContext, cat::RecordError>
    create_remote_context(mem::BufPool &destination_pool, cat::Transaction *parent = nullptr) noexcept;
    [[nodiscard]] std::expected<cat::Event, cat::RecordError> start_event(std::string_view type,
                                                                          std::string_view name) noexcept;
    [[nodiscard]] std::expected<cat::Transaction, cat::RecordError> start_transaction(std::string_view type,
                                                                                      std::string_view name) noexcept;

private:
    http::HttpExchange *exchange_ = nullptr;
    std::optional<cat::Transaction> root_;
    std::optional<cat::MessageTraceContext> context_;
    std::string_view trace_state_;
    std::string_view user_agent_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_CAT_REQUEST_H
