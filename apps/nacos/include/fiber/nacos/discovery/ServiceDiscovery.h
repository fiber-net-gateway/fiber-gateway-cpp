#ifndef FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H
#define FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H

#include "BasicServiceDiscovery.h"
#include "ServiceLoadBalancer.h"

namespace fiber::nacos {

using ServiceDiscovery = BasicServiceDiscovery<LoadBalancerOps>;
using ServiceDiscoveryObserver = ServiceDiscovery::Observer;
using ServiceDiscoveryOptions = LoadBalancerOps::Options;

} // namespace fiber::nacos

#endif // FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H
