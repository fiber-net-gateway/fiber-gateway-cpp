#ifndef FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H
#define FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../config/Ast.h"

namespace fiber::lite_nginx::runtime {

struct RuntimeError {
    std::string message;
    config::SourceLocation location;
};

struct TlsIdentityRuntime {
    std::string server_name;
    std::string certificate;
    std::string certificate_key;
};

struct ServerRuntime {
    config::SourceLocation location;
    std::vector<std::string> server_names;
    std::string certificate;
    std::string certificate_key;
};

struct ListenerRuntime {
    config::SourceLocation location;
    std::string host;
    std::uint16_t port = 0;
    bool has_host = false;
    bool tls = false;
    std::string default_certificate;
    std::string default_certificate_key;
    std::vector<TlsIdentityRuntime> tls_identities;
};

struct RuntimeConfig {
    std::size_t worker_processes = 1;
    std::vector<ServerRuntime> servers;
    std::vector<ListenerRuntime> listeners;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_RUNTIME_CONFIG_H
