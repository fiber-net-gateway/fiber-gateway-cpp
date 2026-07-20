#ifndef FIBER_NACOS_RPC_NACOS_REQUEST_HANDLER_H
#define FIBER_NACOS_RPC_NACOS_REQUEST_HANDLER_H

#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string_view>

#include <common/IoError.h>
#include <common/mem/BufPool.h>
#include <fiber/nacos/dto/Config.h>
#include <nacos_grpc_payload.pb.h>

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
        headers_(metadata), module_(module), pool_(&pool) {}

    [[nodiscard]] const NacosHeadersView &headers() const noexcept { return headers_; }
    [[nodiscard]] std::string_view module() const noexcept { return module_; }
    [[nodiscard]] mem::BufPool &pool() const noexcept { return *pool_; }

    // Response DTOs store string_view fields. Use this helper when the value is
    // not static and does not already live in this callback's decode pool.
    [[nodiscard]] common::IoResult<std::string_view> copy_to_pool(std::string_view value) noexcept {
        if (value.empty()) {
            return std::string_view{};
        }
        auto *storage = static_cast<char *>(pool_->alloc(value.size(), alignof(char)));
        if (!storage) {
            return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(storage, value.data(), value.size());
        return std::string_view(storage, value.size());
    }

private:
    NacosHeadersView headers_;
    std::string_view module_;
    mem::BufPool *pool_ = nullptr;
};

struct NacosServerHandlerError {
    std::int32_t result_code = dto::kResponseFail;
    std::int32_t error_code = dto::kResponseFail;
    std::string_view message;
};

template<typename Request>
struct NacosServerRequestTraits;

template<>
struct NacosServerRequestTraits<dto::req::ConfigChangeNotifyRequest> {
    using Response = dto::resp::ConfigChangeNotifyResponse;
};

template<typename Request>
using NacosServerResponse = typename NacosServerRequestTraits<Request>::Response;

template<typename Request>
using RequestHandler = std::expected<void, NacosServerHandlerError> (*)(
        void *context, NacosServerRequestContext &request_context, const Request &request,
        NacosServerResponse<Request> &response) noexcept;

enum class NacosHandlerRegistrationError : std::uint8_t {
    InvalidHandler,
    DuplicateType,
    RegistryFull,
    Started,
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_REQUEST_HANDLER_H
