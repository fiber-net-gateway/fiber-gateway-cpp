#include "TextEncoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>

#include "PrometheusInternal.h"

namespace fiber::prometheus::detail {

namespace {

using fiber::common::IoErr;

class ChainWriter {
public:
    ChainWriter(fiber::mem::IoBufNodePool &node_pool, CollectOptions options) noexcept :
        chain_(node_pool), chunk_size_(options.chunk_size), max_bytes_(options.max_output_bytes) {}

    [[nodiscard]] IoErr write(std::string_view value) noexcept {
        if (value.size() > max_bytes_ - written_) {
            return IoErr::MessageTooLarge;
        }
        while (!value.empty()) {
            fiber::mem::IoBuf *tail = chain_.back();
            if (!tail || tail->writable() == 0) {
                fiber::mem::IoBuf chunk = fiber::mem::IoBuf::allocate(chunk_size_);
                if (!chunk || !chain_.append(std::move(chunk))) {
                    return IoErr::NoMem;
                }
                tail = chain_.back();
            }
            const std::size_t count = std::min(value.size(), tail->writable());
            std::memcpy(tail->writable_data(), value.data(), count);
            chain_.commit_back(count);
            written_ += count;
            value.remove_prefix(count);
        }
        return IoErr::None;
    }

    [[nodiscard]] fiber::mem::IoBufChain take() noexcept { return std::move(chain_); }

private:
    fiber::mem::IoBufChain chain_;
    std::size_t chunk_size_ = 0;
    std::size_t max_bytes_ = 0;
    std::size_t written_ = 0;
};

class FixedWriter {
public:
    FixedWriter(fiber::mem::IoBuf &out, CollectOptions options) noexcept :
        destination_(out.writable_data()), capacity_(out.writable()), max_bytes_(options.max_output_bytes) {}

    [[nodiscard]] IoErr write(std::string_view value) noexcept {
        const std::size_t limit = std::min(capacity_, max_bytes_);
        if (value.size() > limit - written_) {
            return IoErr::MessageTooLarge;
        }
        std::memcpy(destination_ + written_, value.data(), value.size());
        written_ += value.size();
        return IoErr::None;
    }

    [[nodiscard]] std::size_t written() const noexcept { return written_; }

private:
    std::uint8_t *destination_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t max_bytes_ = 0;
    std::size_t written_ = 0;
};

template<typename Writer>
IoErr write_uint(Writer &writer, std::uint64_t value) noexcept {
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2> buffer{};
    auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        return IoErr::Invalid;
    }
    return writer.write(std::string_view(buffer.data(), static_cast<std::size_t>(end - buffer.data())));
}

template<typename Writer>
IoErr write_int(Writer &writer, std::int64_t value) noexcept {
    std::array<char, std::numeric_limits<std::int64_t>::digits10 + 3> buffer{};
    auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        return IoErr::Invalid;
    }
    return writer.write(std::string_view(buffer.data(), static_cast<std::size_t>(end - buffer.data())));
}

std::uint64_t unit_divisor(HistogramUnit unit) noexcept {
    switch (unit) {
        case HistogramUnit::Nanoseconds:
            return 1'000'000'000;
        case HistogramUnit::Microseconds:
            return 1'000'000;
        case HistogramUnit::Milliseconds:
            return 1'000;
        case HistogramUnit::Raw:
        case HistogramUnit::Seconds:
            return 1;
    }
    return 1;
}

std::size_t decimal_places(HistogramUnit unit) noexcept {
    switch (unit) {
        case HistogramUnit::Nanoseconds:
            return 9;
        case HistogramUnit::Microseconds:
            return 6;
        case HistogramUnit::Milliseconds:
            return 3;
        case HistogramUnit::Raw:
        case HistogramUnit::Seconds:
            return 0;
    }
    return 0;
}

template<typename Writer>
IoErr write_histogram_number(Writer &writer, std::uint64_t value, HistogramUnit unit) noexcept {
    if (unit == HistogramUnit::Raw || unit == HistogramUnit::Seconds) {
        return write_uint(writer, value);
    }
    const std::uint64_t divisor = unit_divisor(unit);
    IoErr error = write_uint(writer, value / divisor);
    if (error != IoErr::None || value % divisor == 0) {
        return error;
    }
    if ((error = writer.write(".")) != IoErr::None) {
        return error;
    }

    std::array<char, 9> fraction{};
    std::uint64_t remainder = value % divisor;
    const std::size_t places = decimal_places(unit);
    for (std::size_t index = places; index > 0; --index) {
        fraction[index - 1] = static_cast<char>('0' + remainder % 10);
        remainder /= 10;
    }
    std::size_t length = places;
    while (length > 0 && fraction[length - 1] == '0') {
        --length;
    }
    return writer.write(std::string_view(fraction.data(), length));
}

template<typename Writer>
IoErr write_help(Writer &writer, std::string_view help) noexcept {
    std::size_t start = 0;
    for (std::size_t index = 0; index < help.size(); ++index) {
        if (help[index] != '\\' && help[index] != '\n') {
            continue;
        }
        IoErr error = writer.write(help.substr(start, index - start));
        if (error != IoErr::None) {
            return error;
        }
        if ((error = writer.write(help[index] == '\\' ? "\\\\" : "\\n")) != IoErr::None) {
            return error;
        }
        start = index + 1;
    }
    return writer.write(help.substr(start));
}

template<typename Writer>
IoErr write_label_value(Writer &writer, std::string_view value) noexcept {
    std::size_t start = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch != '\\' && ch != '"' && ch != '\n') {
            continue;
        }
        IoErr error = writer.write(value.substr(start, index - start));
        if (error != IoErr::None) {
            return error;
        }
        const std::string_view escaped = ch == '\\' ? "\\\\" : (ch == '"' ? "\\\"" : "\\n");
        if ((error = writer.write(escaped)) != IoErr::None) {
            return error;
        }
        start = index + 1;
    }
    return writer.write(value.substr(start));
}

template<typename Writer>
IoErr write_labels(Writer &writer, const FamilySchema &family, const SeriesSchema &series) noexcept {
    if (family.label_names.empty()) {
        return IoErr::None;
    }
    IoErr error = writer.write("{");
    for (std::size_t index = 0; error == IoErr::None && index < family.label_names.size(); ++index) {
        if (index != 0) {
            error = writer.write(",");
        }
        if (error == IoErr::None) {
            error = writer.write(family.label_names[index]);
        }
        if (error == IoErr::None) {
            error = writer.write("=\"");
        }
        if (error == IoErr::None) {
            error = write_label_value(writer, series.label_values[index]);
        }
        if (error == IoErr::None) {
            error = writer.write("\"");
        }
    }
    if (error != IoErr::None) {
        return error;
    }
    return writer.write("}");
}

template<typename Writer>
IoErr write_histogram_labels(Writer &writer, const FamilySchema &family, const SeriesSchema &series,
                             const std::uint64_t *upper_bound) noexcept {
    IoErr error = writer.write("{");
    for (std::size_t index = 0; error == IoErr::None && index < family.label_names.size(); ++index) {
        if (index != 0) {
            error = writer.write(",");
        }
        if (error == IoErr::None) {
            error = writer.write(family.label_names[index]);
        }
        if (error == IoErr::None) {
            error = writer.write("=\"");
        }
        if (error == IoErr::None) {
            error = write_label_value(writer, series.label_values[index]);
        }
        if (error == IoErr::None) {
            error = writer.write("\"");
        }
    }
    if (error == IoErr::None && !family.label_names.empty()) {
        error = writer.write(",");
    }
    if (error == IoErr::None) {
        error = writer.write("le=\"");
    }
    if (error == IoErr::None) {
        error = upper_bound ? write_histogram_number(writer, *upper_bound, family.histogram_unit)
                            : writer.write("+Inf");
    }
    if (error == IoErr::None) {
        error = writer.write("\"}");
    }
    return error;
}

template<typename Writer>
IoErr write_metadata(Writer &writer, const FamilySchema &family) noexcept {
    IoErr error = writer.write("# HELP ");
    if (error == IoErr::None) {
        error = writer.write(family.name);
    }
    if (error == IoErr::None) {
        error = writer.write(" ");
    }
    if (error == IoErr::None) {
        error = write_help(writer, family.help);
    }
    if (error == IoErr::None) {
        error = writer.write("\n# TYPE ");
    }
    if (error == IoErr::None) {
        error = writer.write(family.name);
    }
    if (error == IoErr::None) {
        switch (family.type) {
            case MetricType::Counter:
                error = writer.write(" counter\n");
                break;
            case MetricType::Gauge:
                error = writer.write(" gauge\n");
                break;
            case MetricType::Histogram:
                error = writer.write(" histogram\n");
                break;
        }
    }
    return error;
}

bool valid_snapshot(const RegistryData &data, std::size_t offset, std::size_t words) noexcept {
    if (words > data.word_count || offset > data.word_count - words) {
        return false;
    }
    return std::all_of(data.snapshots.begin(), data.snapshots.end(),
                       [&](const std::vector<std::uint64_t> &snapshot) { return snapshot.size() >= data.word_count; });
}

template<typename Writer>
IoErr write_counter(Writer &writer, const RegistryData &data, const FamilySchema &family,
                    const SeriesSchema &series) noexcept {
    if (!valid_snapshot(data, series.word_offset, 1)) {
        return IoErr::Invalid;
    }
    std::uint64_t aggregate = 0;
    for (const auto &snapshot: data.snapshots) {
        aggregate += snapshot[series.word_offset];
    }
    IoErr error = writer.write(family.name);
    if (error == IoErr::None) {
        error = write_labels(writer, family, series);
    }
    if (error == IoErr::None) {
        error = writer.write(" ");
    }
    if (error == IoErr::None) {
        error = write_uint(writer, aggregate);
    }
    if (error == IoErr::None) {
        error = writer.write("\n");
    }
    return error;
}

template<typename Writer>
IoErr write_gauge(Writer &writer, const RegistryData &data, const FamilySchema &family,
                  const SeriesSchema &series) noexcept {
    if (!valid_snapshot(data, series.word_offset, 1)) {
        return IoErr::Invalid;
    }
    std::int64_t aggregate = 0;
    bool first = true;
    for (const auto &snapshot: data.snapshots) {
        const std::int64_t value = std::bit_cast<std::int64_t>(snapshot[series.word_offset]);
        if (first) {
            aggregate = value;
            first = false;
            continue;
        }
        switch (family.reduction) {
            case GaugeReduction::Sum:
                aggregate += value;
                break;
            case GaugeReduction::Min:
                aggregate = std::min(aggregate, value);
                break;
            case GaugeReduction::Max:
                aggregate = std::max(aggregate, value);
                break;
        }
    }
    IoErr error = writer.write(family.name);
    if (error == IoErr::None) {
        error = write_labels(writer, family, series);
    }
    if (error == IoErr::None) {
        error = writer.write(" ");
    }
    if (error == IoErr::None) {
        error = write_int(writer, aggregate);
    }
    if (error == IoErr::None) {
        error = writer.write("\n");
    }
    return error;
}

template<typename Writer>
IoErr write_histogram(Writer &writer, RegistryData &data, const FamilySchema &family,
                      const SeriesSchema &series) noexcept {
    const std::size_t bucket_count = family.upper_bounds.size();
    if (!valid_snapshot(data, series.word_offset, bucket_count + 2) || data.histogram_scratch.size() < bucket_count) {
        return IoErr::Invalid;
    }
    std::fill_n(data.histogram_scratch.begin(), bucket_count, 0);
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    for (const auto &snapshot: data.snapshots) {
        for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
            data.histogram_scratch[bucket] += snapshot[series.word_offset + bucket];
        }
        count += snapshot[series.word_offset + bucket_count];
        sum += snapshot[series.word_offset + bucket_count + 1];
    }

    std::uint64_t cumulative = 0;
    for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
        cumulative += data.histogram_scratch[bucket];
        IoErr error = writer.write(family.name);
        if (error == IoErr::None) {
            error = writer.write("_bucket");
        }
        if (error == IoErr::None) {
            error = write_histogram_labels(writer, family, series, &family.upper_bounds[bucket]);
        }
        if (error == IoErr::None) {
            error = writer.write(" ");
        }
        if (error == IoErr::None) {
            error = write_uint(writer, cumulative);
        }
        if (error == IoErr::None) {
            error = writer.write("\n");
        }
        if (error != IoErr::None) {
            return error;
        }
    }

    IoErr error = writer.write(family.name);
    if (error == IoErr::None) {
        error = writer.write("_bucket");
    }
    if (error == IoErr::None) {
        error = write_histogram_labels(writer, family, series, nullptr);
    }
    if (error == IoErr::None) {
        error = writer.write(" ");
    }
    if (error == IoErr::None) {
        error = write_uint(writer, count);
    }
    if (error == IoErr::None) {
        error = writer.write("\n");
    }
    if (error == IoErr::None) {
        error = writer.write(family.name);
    }
    if (error == IoErr::None) {
        error = writer.write("_sum");
    }
    if (error == IoErr::None) {
        error = write_labels(writer, family, series);
    }
    if (error == IoErr::None) {
        error = writer.write(" ");
    }
    if (error == IoErr::None) {
        error = write_histogram_number(writer, sum, family.histogram_unit);
    }
    if (error == IoErr::None) {
        error = writer.write("\n");
    }
    if (error == IoErr::None) {
        error = writer.write(family.name);
    }
    if (error == IoErr::None) {
        error = writer.write("_count");
    }
    if (error == IoErr::None) {
        error = write_labels(writer, family, series);
    }
    if (error == IoErr::None) {
        error = writer.write(" ");
    }
    if (error == IoErr::None) {
        error = write_uint(writer, count);
    }
    if (error == IoErr::None) {
        error = writer.write("\n");
    }
    return error;
}

template<typename Writer>
IoErr encode(Writer &writer, RegistryData &data) noexcept {
    for (const auto &family: data.families) {
        if (family.series.empty()) {
            continue;
        }
        IoErr error = write_metadata(writer, family);
        if (error != IoErr::None) {
            return error;
        }
        for (const auto &series: family.series) {
            switch (family.type) {
                case MetricType::Counter:
                    error = write_counter(writer, data, family, series);
                    break;
                case MetricType::Gauge:
                    error = write_gauge(writer, data, family, series);
                    break;
                case MetricType::Histogram:
                    error = write_histogram(writer, data, family, series);
                    break;
            }
            if (error != IoErr::None) {
                return error;
            }
        }
    }
    return IoErr::None;
}

} // namespace

fiber::common::IoResult<fiber::mem::IoBufChain>
encode_text_chain(RegistryData &data, fiber::mem::IoBufNodePool &node_pool, CollectOptions options) noexcept {
    if (options.chunk_size == 0) {
        return std::unexpected(IoErr::Invalid);
    }
    ChainWriter writer(node_pool, options);
    const IoErr error = encode(writer, data);
    if (error != IoErr::None) {
        return std::unexpected(error);
    }
    return writer.take();
}

fiber::common::IoResult<std::size_t> encode_text_into(RegistryData &data, fiber::mem::IoBuf &out,
                                                      CollectOptions options) noexcept {
    if (!out) {
        return std::unexpected(IoErr::Invalid);
    }
    FixedWriter writer(out, options);
    const IoErr error = encode(writer, data);
    if (error != IoErr::None) {
        return std::unexpected(error);
    }
    out.commit(writer.written());
    return writer.written();
}

} // namespace fiber::prometheus::detail
