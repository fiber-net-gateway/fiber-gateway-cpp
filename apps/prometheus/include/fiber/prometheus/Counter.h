#ifndef FIBER_PROMETHEUS_COUNTER_H
#define FIBER_PROMETHEUS_COUNTER_H

#include <cstdint>

namespace fiber::prometheus {

class CounterRef {
public:
    CounterRef() noexcept = default;
    explicit CounterRef(std::uint64_t &value) noexcept : value_(&value) {}

    [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }

    void inc() noexcept { ++*value_; }
    void add(std::uint64_t delta) noexcept { *value_ += delta; }

private:
    std::uint64_t *value_ = nullptr;
};

} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_COUNTER_H
