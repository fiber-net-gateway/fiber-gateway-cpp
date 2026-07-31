#ifndef FIBER_AI_SERVER_SERVICE_DISCOVERY_H
#define FIBER_AI_SERVER_SERVICE_DISCOVERY_H

#include "LoadBalancer.h"

#include <fiber/nacos/discovery/ServiceDiscovery.h>

namespace fiber::ai_server {

using nacos::ServiceDiscovery;
using nacos::ServiceDiscoveryObserver;
using nacos::ServiceDiscoveryOptions;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_SERVICE_DISCOVERY_H
