#ifndef FIBER_AI_SERVER_MCP_SCRIPT_SERVICES_H
#define FIBER_AI_SERVER_MCP_SCRIPT_SERVICES_H

#include "../provider/WorkerDnsService.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <async/Task.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/NamingService.h>
#include <http/LocalHttp1ConnectionPoolSet.h>
#include <http_script/HttpScriptServices.h>

namespace fiber::ai_server {

class McpScriptServices final : public http_script::HttpScriptServices,
                                public common::NonCopyable,
                                public common::NonMovable {
public:
    McpScriptServices(event::EventLoop &nacos_loop, nacos::NamingService &naming_service,
                      event::EventLoopGroup &workers, std::string local_zone) noexcept;
    ~McpScriptServices() override;

    void start_nacos() noexcept;
    [[nodiscard]] async::Task<void> shutdown_nacos() noexcept;
    [[nodiscard]] async::Task<bool> init_workers() noexcept;
    [[nodiscard]] async::Task<void> shutdown_workers() noexcept;

    // Called by the serial MCP tool compiler on the Nacos loop.
    [[nodiscard]] bool observe_target(const http_script::HttpTargetSpec &target) noexcept;

    [[nodiscard]] async::Task<common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
    acquire(const http_script::HttpTargetSpec &target, std::chrono::milliseconds connect_timeout) noexcept override;

private:
    struct Endpoint {
        net::IpAddress ip;
        std::string authority;
        std::string pool_name;
        std::uint16_t port = 0;
        std::uint32_t weight = 1;
        bool tls = false;
    };
    struct DirectoryEntry {
        std::string key;
        std::vector<Endpoint> endpoints;
    };
    struct DirectorySnapshot {
        std::vector<DirectoryEntry> entries;
    };
    struct ServiceWatch;

    static void service_notify(void *context, const nacos::SubscriptionResult<nacos::ServiceInfo> &result) noexcept;
    void apply_service(ServiceWatch &watch, const nacos::ServiceInfo &info);
    void publish_directory();
    [[nodiscard]] std::vector<Endpoint> resolve_target(const http_script::HttpTargetSpec &target) const;

    event::EventLoop *nacos_loop_ = nullptr;
    nacos::NamingService *naming_service_ = nullptr;
    event::EventLoopGroup *workers_ = nullptr;
    std::string local_zone_;
    WorkerDnsService dns_;
    http::LocalHttp1ConnectionPoolSet pool_;
    std::map<std::string, std::unique_ptr<ServiceWatch>, std::less<>> watches_;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const DirectorySnapshot>> directory_;
#else
    std::shared_ptr<const DirectorySnapshot> directory_;
#endif
    std::atomic<std::uint64_t> selection_{0};
    bool nacos_started_ = false;
    bool pool_initialized_ = false;
    bool workers_initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_SCRIPT_SERVICES_H
