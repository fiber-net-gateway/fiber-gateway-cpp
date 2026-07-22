#include "CatSystemMessage.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>

#include "CatInternal.h"

namespace fiber::cat::detail {

namespace {

inline constexpr std::size_t kHeartbeatStorageCapacity = 64 * 1024;

class XmlWriter {
public:
    struct Checkpoint {
        std::size_t size = 0;
        std::size_t field_count = 0;
    };

    XmlWriter(char *storage, std::size_t capacity, std::size_t max_fields) noexcept :
        storage_(storage), capacity_(capacity), max_fields_(max_fields) {}

    bool text(std::string_view value) noexcept {
        if (value.size() > capacity_ - size_) {
            return false;
        }
        std::copy(value.begin(), value.end(), storage_ + size_);
        size_ += value.size();
        return true;
    }

    bool escaped(std::string_view value) noexcept {
        for (const char character: value) {
            std::string_view replacement;
            switch (character) {
                case '&':
                    replacement = "&amp;";
                    break;
                case '<':
                    replacement = "&lt;";
                    break;
                case '>':
                    replacement = "&gt;";
                    break;
                case '\"':
                    replacement = "&quot;";
                    break;
                case '\'':
                    replacement = "&apos;";
                    break;
                default:
                    if (static_cast<unsigned char>(character) < 0x20 && character != '\t') {
                        return false;
                    }
                    replacement = {&character, 1};
                    break;
            }
            if (!text(replacement)) {
                return false;
            }
        }
        return true;
    }

    bool detail(std::string_view key, std::string_view value) noexcept {
        const Checkpoint saved = checkpoint();
        if (field_count_ >= max_fields_ || !text("<extensionDetail id=\"") || !escaped(key) || !text("\" value=\"") ||
            !escaped(value) || !text("\"/>")) {
            rollback(saved);
            return false;
        }
        ++field_count_;
        return true;
    }

    bool detail(std::string_view key, std::uint64_t value) noexcept {
        std::array<char, 32> number{};
        auto result = std::to_chars(number.data(), number.data() + number.size(), value);
        return result.ec == std::errc{} &&
               detail(key, {number.data(), static_cast<std::size_t>(result.ptr - number.data())});
    }

    bool detail(std::string_view key, bool value) noexcept {
        return detail(key, value ? std::string_view("true") : std::string_view("false"));
    }

    bool decimal_detail(std::string_view key, double value) noexcept {
        if (!std::isfinite(value)) {
            return false;
        }
        std::array<char, 48> number{};
        auto result = std::to_chars(number.data(), number.data() + number.size(), value, std::chars_format::fixed, 2);
        return result.ec == std::errc{} &&
               detail(key, {number.data(), static_cast<std::size_t>(result.ptr - number.data())});
    }

    [[nodiscard]] Checkpoint checkpoint() const noexcept { return {.size = size_, .field_count = field_count_}; }

    void rollback(Checkpoint checkpoint) noexcept {
        size_ = checkpoint.size;
        field_count_ = checkpoint.field_count;
    }

    [[nodiscard]] std::string_view view() const noexcept { return {storage_, size_}; }

private:
    char *storage_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t max_fields_ = 0;
    std::size_t size_ = 0;
    std::size_t field_count_ = 0;
};

bool write_system_stats(XmlWriter &writer, const HeartbeatSystemStats *stats) noexcept {
    if (!stats || !stats->has_values()) {
        return true;
    }
    const XmlWriter::Checkpoint saved = writer.checkpoint();
    bool written =
            writer.text("<extension id=\"system.process\"><description><![CDATA[system.process]]></description>");
    if (stats->load_valid) {
        written = written && writer.decimal_detail("system.load.average", stats->load_1min) &&
                  writer.decimal_detail("load.1min", stats->load_1min) &&
                  writer.decimal_detail("load.5min", stats->load_5min) &&
                  writer.decimal_detail("load.15min", stats->load_15min);
    }
    if (stats->scheduler_valid) {
        written = written && writer.detail("process.running", stats->processes_running) &&
                  writer.detail("process.blocked", stats->processes_blocked);
    }
    if (stats->cpu_valid) {
        written = written && writer.detail("cpu.user", stats->cpu_delta.user) &&
                  writer.detail("cpu.nice", stats->cpu_delta.nice) &&
                  writer.detail("cpu.system", stats->cpu_delta.system) &&
                  writer.detail("cpu.idle", stats->cpu_delta.idle) &&
                  writer.detail("cpu.iowait", stats->cpu_delta.iowait) &&
                  writer.detail("cpu.irq", stats->cpu_delta.irq) &&
                  writer.detail("cpu.softirq", stats->cpu_delta.softirq) &&
                  writer.decimal_detail("cpu.user.percent", stats->cpu_user_percent) &&
                  writer.decimal_detail("cpu.nice.percent", stats->cpu_nice_percent) &&
                  writer.decimal_detail("cpu.system.percent", stats->cpu_system_percent) &&
                  writer.decimal_detail("cpu.idle.percent", stats->cpu_idle_percent) &&
                  writer.decimal_detail("cpu.iowait.percent", stats->cpu_iowait_percent) &&
                  writer.decimal_detail("cpu.irq.percent", stats->cpu_irq_percent) &&
                  writer.decimal_detail("cpu.softirq.percent", stats->cpu_softirq_percent);
    }
    if (stats->scheduler_delta_valid) {
        written = written && writer.detail("cpu.context", stats->context_switches_delta) &&
                  writer.detail("cpu.intr", stats->interrupts_delta);
    }
    if (stats->memory_valid) {
        written = written && writer.detail("mem.memtotal", stats->memory_total_bytes) &&
                  writer.detail("mem.memfree", stats->memory_free_bytes) &&
                  writer.detail("mem.memcached", stats->memory_cached_bytes) &&
                  writer.detail("mem.swaptotal", stats->swap_total_bytes) &&
                  writer.detail("mem.swapfree", stats->swap_free_bytes) &&
                  writer.decimal_detail("mem.memfree.percent", stats->memory_free_percent) &&
                  writer.decimal_detail("mem.memused.percent", stats->memory_used_percent);
    }
    if (stats->process_memory_valid) {
        written = written && writer.detail("process.rss.bytes", stats->process_rss_bytes) &&
                  writer.detail("process.virtual.bytes", stats->process_virtual_bytes);
    }
    if (stats->process_cpu_valid) {
        written = written && writer.decimal_detail("process.cpu.user.percent", stats->process_cpu_user_percent) &&
                  writer.decimal_detail("process.cpu.system.percent", stats->process_cpu_system_percent) &&
                  writer.decimal_detail("process.cpu.total.percent", stats->process_cpu_total_percent);
    }
    written = written && writer.text("</extension>");
    if (!written) {
        writer.rollback(saved);
    }
    return written;
}

void freeze_transaction_tree(TransactionData &root) noexcept {
    std::size_t visited = 0;
    for (ChildrenChunk *chunk = root.children_head; chunk; chunk = chunk->next) {
        const std::size_t count = std::min(kChildrenPerChunk, root.child_count - visited);
        for (std::size_t index = 0; index < count; ++index) {
            chunk->children[index]->completed = true;
        }
        visited += count;
    }
    root.duration = std::chrono::microseconds::zero();
    root.explicit_duration = true;
    root.completed = true;
    root.trace->data->open_message_count = 0;
}

std::expected<mem::IoBuf, EncodeError> encode_tree(MessageTrace *trace, const ClientEncodeContext &client,
                                                   CatEncoderType encoder) noexcept {
    auto encoded = encode_message_tree(*trace->data, client, encoder);
    delete trace;
    return encoded;
}

} // namespace

std::expected<mem::IoBuf, EncodeError> encode_startup_nt1(const ClientEncodeContext &client,
                                                          std::string_view message_id, std::string_view ip,
                                                          std::string_view client_version,
                                                          CatEncoderType encoder) noexcept {
    RecordLimits limits;
    limits.max_messages = 3;
    limits.max_children_per_transaction = 2;
    auto root_created = create_transaction_root("System", "Reboot", limits, {.message_id = message_id});
    if (!root_created) {
        return std::unexpected(root_created.error() == RecordError::NoMemory ? EncodeError::NoMemory
                                                                             : EncodeError::InvalidTrace);
    }
    TransactionData *root = *root_created;
    auto reboot = create_event(*root, "Reboot", ip);
    auto version = create_event(*root, "Cat_Fiber2_Client_Version", client_version);
    if (!reboot || !version) {
        delete root->trace;
        return std::unexpected(EncodeError::NoMemory);
    }
    freeze_transaction_tree(*root);
    return encode_tree(root->trace, client, encoder);
}

std::expected<mem::IoBuf, EncodeError> encode_heartbeat_nt1(const ClientEncodeContext &client,
                                                            std::string_view message_id, const HeartbeatInfo &info,
                                                            std::size_t max_fields, std::size_t max_data_bytes,
                                                            CatEncoderType encoder) noexcept {
    if (max_data_bytes == 0 || max_data_bytes > kHeartbeatStorageCapacity) {
        return std::unexpected(EncodeError::SizeOverflow);
    }
    std::array<char, kHeartbeatStorageCapacity> storage{};
    XmlWriter writer(storage.data(), max_data_bytes, max_fields);
    const CatClientStats &stats = info.stats;
    if (!writer.text("<status><extension id=\"fiber2.cat\">") || !writer.detail("app.key", info.app_key) ||
        !writer.detail("host.name", info.hostname) || !writer.detail("host.ip", info.ip) ||
        !writer.detail("client.version", info.client_version) || !writer.detail("process.id", info.process_id) ||
        !writer.detail("process.start.millis", info.process_start_millis) ||
        !writer.detail("process.uptime.millis", info.uptime_millis) ||
        !writer.detail("event.loop.count", info.event_loop_count) ||
        !writer.detail("collector.count", info.collector_count) ||
        !writer.detail("collector.connected", info.collector_connected) ||
        !writer.detail("router.last.success.millis", info.router_last_success_millis) ||
        !writer.detail("router.blocked", info.blocked) || !writer.detail("router.sample.cutoff", info.sample_cutoff) ||
        !writer.detail("queue.messages", stats.queued_messages) || !writer.detail("queue.bytes", stats.queued_bytes) ||
        !writer.detail("messages.submitted", stats.submitted_messages) ||
        !writer.detail("messages.sent", stats.sent_messages) || !writer.detail("bytes.sent", stats.sent_bytes) ||
        !writer.detail("drop.queue.full", stats.dropped_queue_full) ||
        !writer.detail("drop.unavailable", stats.dropped_unavailable) ||
        !writer.detail("drop.partial.frame", stats.dropped_partial_frame) ||
        !writer.detail("fail.encode", stats.encode_failures) || !writer.detail("fail.router", stats.router_failures) ||
        !writer.detail("fail.connect", stats.connect_failures) || !writer.detail("fail.write", stats.write_failures) ||
        !writer.detail("trees.sampled", stats.sampled_trees) ||
        !writer.detail("trees.aggregated", stats.aggregated_trees) ||
        !writer.detail("aggregate.overflow", stats.aggregation_overflow) ||
        !writer.detail("metric.observations", stats.metric_observations) ||
        !writer.detail("heartbeat.provider.failures", stats.heartbeat_provider_failures) ||
        !writer.text("</extension>")) {
        return std::unexpected(EncodeError::SizeOverflow);
    }
    (void) write_system_stats(writer, info.system_stats);
    if (!writer.text("</status>")) {
        return std::unexpected(EncodeError::SizeOverflow);
    }

    RecordLimits limits;
    limits.max_messages = 2;
    limits.max_children_per_transaction = 1;
    limits.max_data_bytes_per_message = max_data_bytes;
    limits.max_tree_bytes = max_data_bytes + 16 * 1024;
    auto root_created = create_transaction_root("System", "Status", limits, {.message_id = message_id});
    if (!root_created) {
        return std::unexpected(root_created.error() == RecordError::NoMemory ? EncodeError::NoMemory
                                                                             : EncodeError::InvalidTrace);
    }
    TransactionData *root = *root_created;
    auto heartbeat = create_heartbeat(*root, "Heartbeat", info.ip);
    if (!heartbeat || add_data(*heartbeat, writer.view()) != RecordError::None) {
        delete root->trace;
        return std::unexpected(EncodeError::NoMemory);
    }
    if (info.timestamp_millis != 0 && (set_timestamp(root, info.timestamp_millis) != RecordError::None ||
                                       set_timestamp(*heartbeat, info.timestamp_millis) != RecordError::None)) {
        delete root->trace;
        return std::unexpected(EncodeError::InvalidTrace);
    }
    freeze_transaction_tree(*root);
    return encode_tree(root->trace, client, encoder);
}

} // namespace fiber::cat::detail
