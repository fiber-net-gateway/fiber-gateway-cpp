#include "McpToolLoader.h"

#include "McpJsonCodec.h"
#include "McpScriptRuntime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

#include <common/Assert.h>
#include <common/mem/BufPool.h>
#include <http/ClientHttp1Exchange.h>
#include <http/ClientHttp1Types.h>
#include <http/Http1ConnectionGroupKey.h>
#include <http/HttpBodySpec.h>
#include <http/HttpHeaders.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {
namespace {

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kAdminTimeout{10s};
constexpr std::size_t kMaxAdminBodyBytes = 4 * 1024 * 1024;

bool is_default_cluster(std::string_view cluster) noexcept {
    const std::size_t separator = cluster.find('-');
    if (separator != std::string_view::npos) {
        cluster.remove_prefix(separator + 1);
    }
    constexpr std::string_view expected = "default";
    if (cluster.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < cluster.size(); ++i) {
        char ch = cluster[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (ch != expected[i]) {
            return false;
        }
    }
    return true;
}

McpToolLoadError load_error(McpToolLoadErrorCode code, std::string message,
                            common::IoErr io_error = common::IoErr::None, int status = 0) {
    return McpToolLoadError{
            .code = code,
            .io_error = io_error,
            .http_status = status,
            .message = std::move(message),
    };
}

std::optional<std::string> read_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::nullopt;
    }
    const std::streampos end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxAdminBodyBytes) {
        return std::nullopt;
    }
    std::string content(static_cast<std::size_t>(end), '\0');
    input.seekg(0);
    if (!content.empty() && !input.read(content.data(), static_cast<std::streamsize>(content.size()))) {
        return std::nullopt;
    }
    return content;
}

void save_file(const std::filesystem::path &directory, const McpLoadedTool &tool) noexcept {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return;
    }
    const std::filesystem::path final_path = directory / (tool.descriptor.script_id + ".dat");
    const std::filesystem::path temporary_path = directory / (tool.descriptor.script_id + ".dat.tmp");
    const std::string content = encode_mcp_tool_cache(tool);
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
        output.close();
        std::filesystem::remove(temporary_path, error);
        return;
    }
    output.close();
    std::filesystem::rename(temporary_path, final_path, error);
    if (error) {
        std::filesystem::remove(final_path, error);
        error.clear();
        std::filesystem::rename(temporary_path, final_path, error);
    }
    if (error) {
        std::filesystem::remove(temporary_path, error);
    }
}

std::string host_header(const McpToolLoader::AdminNode &node) {
    std::string output = node.host;
    output.push_back(':');
    output.append(std::to_string(node.port));
    return output;
}

} // namespace

McpToolLoader::McpToolLoader(event::EventLoop &loop, nacos::NamingService &naming_service,
                             std::filesystem::path cache_directory, http_script::HttpScriptServices *script_services) :
    loop_(&loop), naming_service_(&naming_service), cache_directory_(std::move(cache_directory)),
    script_services_(script_services), pool_(loop, http::Http1ConnectionPoolCore::Options{
                                                           .max_idle_per_group = 2,
                                                           .max_idle_total = 16,
                                                           .idle_timeout = std::chrono::seconds(30),
                                                           .initial_group_capacity = 4,
                                                   }) {}

McpToolLoader::~McpToolLoader() { FIBER_ASSERT(!started_); }

std::expected<void, nacos::NamingServiceError> McpToolLoader::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (started_) {
        return {};
    }
    if (!pool_.init()) {
        return std::unexpected(nacos::NamingServiceError{
                .code = nacos::NamingServiceErrorCode::Server,
                .io_error = common::IoErr::NoMem,
                .message = "failed to initialize MCP admin connection pool",
        });
    }
    pool_initialized_ = true;
    auto subscription = naming_service_->subscribe(kMcpAdminServiceName, kMcpAdminServiceGroup, &admin_notify, this);
    if (!subscription) {
        pool_.shutdown();
        pool_initialized_ = false;
        return std::unexpected(std::move(subscription.error()));
    }
    admin_subscription_.emplace(std::move(*subscription));
    started_ = true;
    if (pending_admin_) {
        apply_admin(*pending_admin_);
        pending_admin_.reset();
    }
    return {};
}

void McpToolLoader::admin_notify(void *context, const nacos::SubscriptionResult<nacos::ServiceInfo> &result) noexcept {
    auto &self = *static_cast<McpToolLoader *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        self.admin_nodes_.clear();
        return;
    }
    if (!result.data) {
        return;
    }
    if (!self.started_) {
        self.pending_admin_ = result.data;
        return;
    }
    self.apply_admin(*result.data);
}

void McpToolLoader::apply_admin(const nacos::ServiceInfo &info) {
    FIBER_ASSERT(loop_->in_loop());
    std::vector<AdminNode> nodes;
    nodes.reserve(info.hosts.size());
    for (const nacos::ServiceInstance &instance: info.hosts) {
        if (!instance.enabled || !instance.healthy || instance.port == 0 || !std::isfinite(instance.weight) ||
            instance.weight <= 0 || (!instance.cluster_name.empty() && !is_default_cluster(instance.cluster_name))) {
            continue;
        }
        net::IpAddress ip;
        if (!net::IpAddress::parse(instance.ip, ip) || ip.is_unspecified() || ip.is_multicast()) {
            continue;
        }
        nodes.push_back(AdminNode{
                .ip = ip,
                .host = std::string(instance.ip),
                .port = instance.port,
        });
    }
    admin_nodes_ = std::move(nodes);
    if (next_admin_ >= admin_nodes_.size()) {
        next_admin_ = 0;
    }
}

std::optional<McpToolLoader::AdminNode> McpToolLoader::select_admin() {
    FIBER_ASSERT(loop_->in_loop());
    if (admin_nodes_.empty()) {
        return std::nullopt;
    }
    AdminNode node = admin_nodes_[next_admin_++ % admin_nodes_.size()];
    return node;
}

async::Task<std::expected<std::string, McpToolLoadError>> McpToolLoader::fetch_admin(std::string script_id) noexcept {
    auto selected = select_admin();
    if (!selected) {
        co_return std::unexpected(
                load_error(McpToolLoadErrorCode::NoAdminInstance, "ploto-admin-app has no healthy DEFAULT instance"));
    }
    const auto key = http::Http1ConnectionGroupKey::from_ip(selected->ip, selected->port,
                                                            http::Http1ConnectionGroupKey::Scheme::Http);
    auto lease = pool_.acquire(key);
    if (!lease.valid()) {
        co_return std::unexpected(
                load_error(McpToolLoadErrorCode::PoolUnavailable, "MCP admin connection pool is unavailable"));
    }
    http::Http1ClientConnection *connection = lease.get();
    if (!connection) {
        auto created = lease.emplace_connection(http::Http1ClientConnectionOptions{
                .peer_addr = net::SocketAddress(selected->ip, selected->port),
        });
        if (!created) {
            co_return std::unexpected(
                    load_error(McpToolLoadErrorCode::Connect, "failed to create admin connection", created.error()));
        }
        connection = *created;
        auto connected = co_await connection->connect(kAdminTimeout);
        if (!connected) {
            co_return std::unexpected(
                    load_error(McpToolLoadErrorCode::Connect, "failed to connect to admin", connected.error()));
        }
    }

    mem::BufPool request_pool;
    http::ClientHttp1Exchange exchange(*connection, request_pool);
    http::HttpHeaders headers(request_pool);
    headers.set("Host", host_header(*selected));
    headers.set_view("Accept", "application/json");
    std::string path = "/ploto-admin-app/aiMcp/getTool/";
    path.append(script_id);
    http::Http1RequestHead head{
            .method = http::HttpMethod::Get,
            .target = path,
            .headers = &headers,
            .body = http::HttpBodySpec::None(),
    };
    auto sent = co_await exchange.send_header(head, true, kAdminTimeout);
    if (!sent) {
        co_return std::unexpected(load_error(McpToolLoadErrorCode::Send, "failed to send admin request", sent.error()));
    }
    const http::Http1ResponseHead *response = nullptr;
    for (;;) {
        auto received = co_await exchange.read_header(kAdminTimeout);
        if (!received) {
            co_return std::unexpected(
                    load_error(McpToolLoadErrorCode::Receive, "failed to receive admin response", received.error()));
        }
        if (!(*received)->is_informational()) {
            response = *received;
            break;
        }
    }
    if (!response || response->status_code != 200) {
        co_return std::unexpected(load_error(McpToolLoadErrorCode::HttpStatus, "admin returned non-200 status",
                                             common::IoErr::Invalid, response ? response->status_code : 0));
    }
    std::string body;
    for (;;) {
        const std::size_t remaining = kMaxAdminBodyBytes - body.size();
        auto chunk = co_await exchange.read_body(std::min<std::size_t>(64 * 1024, remaining + 1), kAdminTimeout);
        if (!chunk) {
            co_return std::unexpected(
                    load_error(McpToolLoadErrorCode::Receive, "failed to read admin response", chunk.error()));
        }
        if (chunk->readable_bytes() > remaining) {
            co_return std::unexpected(
                    load_error(McpToolLoadErrorCode::ResponseTooLarge, "admin tool response is too large"));
        }
        while (const mem::IoBuf *part = chunk->first_readable()) {
            const std::size_t size = part->readable();
            body.append(reinterpret_cast<const char *>(part->readable_data()), size);
            chunk->consume_and_compact(size);
        }
        if (chunk->complete()) {
            break;
        }
    }
    co_return body;
}

async::Task<std::expected<std::shared_ptr<const McpTool>, McpToolLoadError>>
McpToolLoader::load(std::string script_id) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!valid_mcp_script_id(script_id)) {
        co_return std::unexpected(load_error(McpToolLoadErrorCode::InvalidId, "invalid MCP tool script id"));
    }
    bool from_admin = false;
    std::expected<McpLoadedTool, McpJsonError> loaded = std::unexpected(McpJsonError{.message = "cache unavailable"});
    const std::filesystem::path cache_path = cache_directory_ / (script_id + ".dat");
    if (auto cache = read_file(cache_path)) {
        loaded = parse_mcp_tool_cache(*cache, script_id);
    }
    if (!loaded) {
        auto response = co_await fetch_admin(script_id);
        if (!response) {
            co_return std::unexpected(std::move(response.error()));
        }
        loaded = parse_mcp_admin_tool(*response, script_id);
        from_admin = true;
    }
    if (!loaded) {
        co_return std::unexpected(load_error(McpToolLoadErrorCode::InvalidTool, std::move(loaded.error().message)));
    }
    auto handler = compile_mcp_tool_script(loaded->script, script_services_);
    if (!handler) {
        co_return std::unexpected(load_error(McpToolLoadErrorCode::Compile, std::move(handler.error().message)));
    }
    auto tool = std::make_shared<McpTool>();
    tool->descriptor = loaded->descriptor;
    tool->handler = std::move(*handler);
    if (from_admin) {
        save_file(cache_directory_, *loaded);
    }
    co_return std::static_pointer_cast<const McpTool>(std::move(tool));
}

async::Task<void> McpToolLoader::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    stop();
    co_return;
}

void McpToolLoader::stop() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!started_) {
        return;
    }
    admin_subscription_->close();
    admin_subscription_.reset();
    pending_admin_.reset();
    admin_nodes_.clear();
    if (pool_initialized_) {
        pool_.shutdown();
        pool_initialized_ = false;
    }
    started_ = false;
}

} // namespace fiber::ai_server
