#ifndef FIBER_NACOS_RPC_NACOS_BI_REQUEST_HANDLER_H
#define FIBER_NACOS_RPC_NACOS_BI_REQUEST_HANDLER_H

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>

#include <async/Task.h>
#include <common/Assert.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <common/mem/BufPool.h>
#include <fiber/nacos/dto/Internal.h>
#include <nacos_grpc_payload.pb.h>

#include "NacosPayloadCodec.h"

namespace fiber::nacos::detail {

class NacosHeadersView {
public:
    explicit NacosHeadersView(const proto::Metadata &metadata) noexcept : metadata_(&metadata) {}

    [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const noexcept {
        const auto &headers = metadata_->headers();
        for (const auto &[key, value]: headers) {
            if (key == name) {
                return value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const auto &raw() const noexcept { return metadata_->headers(); }

private:
    const proto::Metadata *metadata_ = nullptr;
};

class NacosServerRequestContext {
public:
    NacosServerRequestContext(const proto::Metadata &metadata, std::string_view module, mem::BufPool &pool) noexcept :
        headers_(metadata), module_(module), pool_(pool) {}

    [[nodiscard]] const NacosHeadersView &headers() const noexcept { return headers_; }
    [[nodiscard]] std::string_view module() const noexcept { return module_; }
    [[nodiscard]] mem::BufPool &pool() const noexcept { return pool_; }

    // Response DTOs store string_view fields. Use this helper when the value is
    // not static and does not already live in this request's pool. The pool
    // remains valid until the asynchronous handler result has been encoded.
    [[nodiscard]] common::IoResult<std::string_view> copy_to_pool(std::string_view value) noexcept {
        if (value.empty()) {
            return std::string_view{};
        }
        auto *storage = static_cast<char *>(pool_.alloc(value.size(), alignof(char)));
        if (!storage) {
            return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(storage, value.data(), value.size());
        return std::string_view(storage, value.size());
    }

private:
    NacosHeadersView headers_;
    std::string_view module_;
    mem::BufPool &pool_;
};

template<typename Request, typename Response>
using RequestHandler = async::Task<common::IoResult<Response>> (*)(void *context,
                                                                   NacosServerRequestContext &request_context,
                                                                   const Request &request) noexcept;

enum class NacosHandlerRegistrationError : std::uint8_t {
    InvalidHandler,
    DuplicateType,
    RegistryFull,
};

class NacosRpc;

// Immutable while any NacosRpc::run() is using it. The registry and every
// callback context must outlive those run() calls. Handlers are serialized on
// the bidirectional stream and must complete in bounded time.
class NacosBiRequestHandler : public common::NonCopyable, public common::NonMovable {
    struct HandlerEntry;

    using ErasedHandler = void (*)() noexcept;
    using InvokeHandler = async::Task<std::expected<proto::Payload, NacosRpcError>> (*)(
            const HandlerEntry &entry, std::string_view module, const NacosPayloadView &payload,
            const proto::Metadata &metadata, const NacosPayloadMetadata &outbound_metadata,
            std::size_t max_payload_bytes) noexcept;

    struct HandlerEntry {
        std::string_view type;
        void *context = nullptr;
        ErasedHandler handler = nullptr;
        InvokeHandler invoke = nullptr;
    };

public:
    static constexpr std::size_t kMaxRequestHandlers = 16;

    template<typename Request, typename Response>
        requires std::derived_from<Request, dto::RequestBase> && std::derived_from<Response, dto::ResponseBase> &&
                 requires {
                     Request::kTypeName;
                     Response::kTypeName;
                 }
    [[nodiscard]] std::expected<void, NacosHandlerRegistrationError>
    add_request_handler(RequestHandler<Request, Response> handler, void *context) noexcept {
        if (!handler) {
            return std::unexpected(NacosHandlerRegistrationError::InvalidHandler);
        }
        for (std::size_t i = 0; i < handler_count_; ++i) {
            if (handlers_[i].type == Request::kTypeName) {
                return std::unexpected(NacosHandlerRegistrationError::DuplicateType);
            }
        }
        if (handler_count_ == handlers_.size()) {
            return std::unexpected(NacosHandlerRegistrationError::RegistryFull);
        }
        handlers_[handler_count_++] = HandlerEntry{
                .type = Request::kTypeName,
                .context = context,
                .handler = reinterpret_cast<ErasedHandler>(handler),
                .invoke = &invoke_registered_handler<Request, Response>,
        };
        return {};
    }

private:
    friend class NacosRpc;

    template<typename Request, typename Response>
    static async::Task<std::expected<proto::Payload, NacosRpcError>>
    invoke_registered_handler(const HandlerEntry &entry, std::string_view module, const NacosPayloadView &payload,
                              const proto::Metadata &metadata, const NacosPayloadMetadata &outbound_metadata,
                              std::size_t max_payload_bytes) noexcept {
        auto handler = reinterpret_cast<RequestHandler<Request, Response>>(entry.handler);
        FIBER_ASSERT(handler != nullptr);

        mem::BufPool pool;
        Request request;
        auto parsed = parse_payload_json(payload, pool, request);
        if (!parsed) {
            co_return std::unexpected(std::move(parsed.error()));
        }

        NacosServerRequestContext request_context(metadata, module, pool);
        auto handled = co_await handler(entry.context, request_context, request);
        if (!handled) {
            common::IoErr error = handled.error();
            if (error == common::IoErr::None) {
                error = common::IoErr::Unknown;
            }
            dto::resp::ErrorResponse error_response;
            error_response.result_code = dto::kResponseFail;
            error_response.error_code = common::io_err_to_errno(error);
            error_response.message.set_present(common::io_err_name(error));
            error_response.request_id = request.request_id;
            co_return encode_payload(error_response, outbound_metadata, max_payload_bytes);
        }

        Response response = std::move(*handled);
        response.request_id = request.request_id;
        co_return encode_payload(response, outbound_metadata, max_payload_bytes);
    }

    [[nodiscard]] const HandlerEntry *find_handler(std::string_view type) const noexcept {
        for (std::size_t i = 0; i < handler_count_; ++i) {
            if (handlers_[i].type == type) {
                return &handlers_[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] async::Task<std::expected<std::optional<proto::Payload>, NacosRpcError>>
    dispatch(std::string_view module, const NacosPayloadView &payload, const proto::Metadata &metadata,
             const NacosPayloadMetadata &outbound_metadata, std::size_t max_payload_bytes) const noexcept {
        const HandlerEntry *entry = find_handler(payload.type);
        if (!entry) {
            co_return std::optional<proto::Payload>{};
        }
        auto response = co_await entry->invoke(*entry, module, payload, metadata, outbound_metadata, max_payload_bytes);
        if (!response) {
            co_return std::unexpected(std::move(response.error()));
        }
        co_return std::optional<proto::Payload>(std::move(*response));
    }

    std::array<HandlerEntry, kMaxRequestHandlers> handlers_{};
    std::size_t handler_count_ = 0;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_BI_REQUEST_HANDLER_H
