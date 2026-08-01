#ifndef FIBER_AI_SERVER_LOAD_BALANCER_H
#define FIBER_AI_SERVER_LOAD_BALANCER_H

#include <fiber/nacos/discovery/ServiceLoadBalancer.h>

namespace fiber::ai_server {

using nacos::DiscoveredInstance;
using nacos::DiscoveredService;
using nacos::InstanceReportOutcome;
using nacos::LoadBalanceError;
using nacos::LoadBalancer;
using nacos::LoadBalancerOps;
using nacos::LoadBalancerStats;
using nacos::LoadBalancerUpdateResult;
using nacos::ServiceInstanceSelection;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LOAD_BALANCER_H
