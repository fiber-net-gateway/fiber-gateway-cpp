#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_LOG_CATEGORIES_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_LOG_CATEGORIES_H

#include <string_view>

namespace fiber::access_server {

inline constexpr std::string_view kAccessServerLoggerRoot = "access_server";
inline constexpr std::string_view kAccessServerLifecycleLogger = "access_server.lifecycle";
inline constexpr std::string_view kAccessServerConfigLogger = "access_server.config";
inline constexpr std::string_view kAccessServerHttpLogger = "access_server.http";
inline constexpr std::string_view kAccessServerAccessLogger = "access_server.access";

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_LOG_CATEGORIES_H
