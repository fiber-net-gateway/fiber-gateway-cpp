#ifndef FIBER_PROMETHEUS_GAUGE_H
#define FIBER_PROMETHEUS_GAUGE_H

#include <cstdint>

namespace fiber::prometheus {

class GaugeRef {
public:
    GaugeRef() noexcept = default;
    explicit GaugeRef(std::int64_t &value) noexcept : value_(&value) {}

    [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }

    void set(std::int64_t value) noexcept { *value_ = value; }
    void inc() noexcept { ++*value_; }
    void add(std::int64_t delta) noexcept { *value_ += delta; }
    void dec() noexcept { --*value_; }

private:
    std::int64_t *value_ = nullptr;
};

} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_GAUGE_H
