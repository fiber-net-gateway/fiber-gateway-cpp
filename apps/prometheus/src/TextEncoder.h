#ifndef FIBER_PROMETHEUS_TEXT_ENCODER_H
#define FIBER_PROMETHEUS_TEXT_ENCODER_H

#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include "fiber/prometheus/MetricsRegistry.h"

namespace fiber::prometheus::detail {

struct RegistryData;

[[nodiscard]] fiber::common::IoResult<fiber::mem::IoBufChain>
encode_text_chain(RegistryData &data, fiber::mem::IoBufNodePool &node_pool, CollectOptions options) noexcept;

[[nodiscard]] fiber::common::IoResult<std::size_t> encode_text_into(RegistryData &data, fiber::mem::IoBuf &out,
                                                                    CollectOptions options) noexcept;

} // namespace fiber::prometheus::detail

#endif // FIBER_PROMETHEUS_TEXT_ENCODER_H
