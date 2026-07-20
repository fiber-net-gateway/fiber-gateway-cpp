#ifndef FIBER_NACOS_NACOS_CLIENT_H
#define FIBER_NACOS_NACOS_CLIENT_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <async/Task.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>

#include "ConfigService.h"
#include "NacosAuthAccess.h"
#include "NacosClientConfig.h"
#include "NamingService.h"

namespace fiber::nacos {
namespace detail {
class NacosClientImpl;
}

enum class NacosCreateErrorCode : std::uint8_t {
    InvalidOptions,
    NoMem,
};

struct NacosCreateError {
    NacosCreateErrorCode code = NacosCreateErrorCode::InvalidOptions;
};

class NacosClient : public common::NonCopyable, public common::NonMovable {
public:
    using AuthSubscriber = async::Watch<NacosAuthAccess>::Subscriber;

    [[nodiscard]] static std::expected<std::unique_ptr<NacosClient>, NacosCreateError>
    create(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options = {});

    ~NacosClient();

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AuthSubscriber subscribe_auth();
    [[nodiscard]] ConfigService &config_service() noexcept;
    [[nodiscard]] NamingService &naming_service() noexcept;
    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] const NacosClientConfig &config() const noexcept;

private:
    explicit NacosClient(std::unique_ptr<detail::NacosClientImpl> impl) noexcept;

    std::unique_ptr<detail::NacosClientImpl> impl_;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_CLIENT_H
