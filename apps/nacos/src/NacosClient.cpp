#include <fiber/nacos/NacosClient.h>

#include <cstdio>

namespace fiber::nacos {

void hello() noexcept { std::fputs("hello\n", stdout); }

} // namespace fiber::nacos
