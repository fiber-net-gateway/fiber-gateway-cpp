#ifndef FIBER_PROMETHEUS_HISTOGRAM_H
#define FIBER_PROMETHEUS_HISTOGRAM_H

#include <cstddef>
#include <cstdint>
#include <span>

#include <fiber/common/Assert.h>

namespace fiber::prometheus {

class HistogramRef {
public:
    HistogramRef() noexcept = default;

    HistogramRef(std::span<const std::uint64_t> upper_bounds, std::span<std::uint64_t> interval_counts,
                 std::uint64_t &count, std::uint64_t &sum) noexcept :
        upper_bounds_(upper_bounds.data()), interval_counts_(interval_counts.data()),
        bucket_count_(upper_bounds.size()), count_(&count), sum_(&sum) {
        FIBER_ASSERT(upper_bounds.size() == interval_counts.size());
    }

    [[nodiscard]] bool valid() const noexcept { return count_ != nullptr; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return bucket_count_; }

    void observe(std::uint64_t value) noexcept {
        std::size_t first = 0;
        std::size_t last = bucket_count_;
        while (first < last) {
            const std::size_t middle = first + (last - first) / 2;
            if (value <= upper_bounds_[middle]) {
                last = middle;
            } else {
                first = middle + 1;
            }
        }
        if (first < bucket_count_) {
            ++interval_counts_[first];
        }
        ++*count_;
        *sum_ += value;
    }

private:
    const std::uint64_t *upper_bounds_ = nullptr;
    std::uint64_t *interval_counts_ = nullptr;
    std::size_t bucket_count_ = 0;
    std::uint64_t *count_ = nullptr;
    std::uint64_t *sum_ = nullptr;
};

} // namespace fiber::prometheus

#endif // FIBER_PROMETHEUS_HISTOGRAM_H
