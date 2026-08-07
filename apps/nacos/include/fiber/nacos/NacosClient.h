#ifndef FIBER_NACOS_NACOS_CLIENT_H
#define FIBER_NACOS_NACOS_CLIENT_H

#include <expected>
#include <memory>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>

#include "NacosAuthAccess.h"
#include "NacosClientConfig.h"
#include "NacosCreateError.h"

namespace fiber::nacos {
namespace detail {
class NacosClientImpl;
struct NacosServiceDependencies;
} // namespace detail

class ConfigService;
class NamingService;

class NacosClient : public common::NonCopyable, public common::NonMovable {
public:
    using AuthSubscriber = NacosAuthSubscriber;

    [[nodiscard]] static std::expected<std::unique_ptr<NacosClient>, NacosCreateError>
    create(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options = {});

    ~NacosClient();

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AuthSubscriber subscribe_auth();
    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] const NacosClientConfig &config() const noexcept;

private:
    friend class ConfigService;
    friend class NamingService;

    explicit NacosClient(std::unique_ptr<detail::NacosClientImpl> impl) noexcept;
    [[nodiscard]] std::expected<detail::NacosServiceDependencies, NacosCreateError> service_dependencies();

    std::unique_ptr<detail::NacosClientImpl> impl_;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_CLIENT_H
