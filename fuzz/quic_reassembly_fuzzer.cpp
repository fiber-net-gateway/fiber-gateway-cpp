// Fuzzes CRYPTO and STREAM receive reassembly (QuicDataReassembler and the
// QuicStreamRecvQueue built on it) against a reference model. Peers control
// every offset/length/FIN/RESET here, and a reassembly bug is silent data
// corruption rather than a crash, so the harness checks content, not just
// memory safety:
//   - every delivered byte is the byte first received at that offset
//     ("first writer wins" for conflicting retransmissions), delivered in
//     order with no gaps;
//   - data is deliverable exactly when the model has the next byte;
//   - the final-size rules of RFC 9000 4.5 (FIN/RESET_STREAM, flow-control
//     limit) accept and reject what the model says they must;
//   - nothing stays charged to the storage budget once the reassembler is
//     gone (the budget is the per-connection memory cap).
//
// Input: [config][op...]
//   config bit 0: target = stream recv queue (else bare reassembler, as used
//                 for CRYPTO); bit 1: charge a storage budget; bits 2-3:
//                 buffer limit {64K, 4K, 512, 64}; bits 4-5: reassembler
//                 extent cap {1024, 16, 4, 1}; bit 6: data arrives as slices
//                 of a larger "datagram" buffer (exercises compaction).
//   op: [kind][...]
//     kind & 3 == 0  data:  [off:u16][len:u16] (+ kind bit 2: off += u16<<16,
//                    kind bit 3: off near 2^62, kind bit 4: conflicting bytes,
//                    kind bit 5: FIN)
//     kind & 3 == 1  take:  [max:u16] (0 = unbounded)
//     kind & 3 == 2  stream: RESET_STREAM [final:u16] (kind bit 2) or
//                    STOP_SENDING; reassembler: discard_buffered()
//     kind & 3 == 3  stream: raise MAX_STREAM_DATA by [u16]<<4

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <sys/uio.h>

#include "QuicFuzzCommon.h"

#include <fiber/common/mem/IoBufChain.h>
#include <fiber/quic/QuicDataReassembler.h>
#include <fiber/quic/QuicStreamRecvQueue.h>

namespace fiber::quic {

struct QuicStreamRecvQueueTestAccess {
    static common::IoResult<std::size_t> recv(QuicStreamRecvQueue &queue, std::uint64_t offset, mem::IoBuf data,
                                              bool fin) noexcept {
        return queue.recv_stream_data(offset, std::move(data), fin);
    }
    static common::IoResult<void> reset(QuicStreamRecvQueue &queue, std::uint64_t final_size) noexcept {
        return queue.recv_reset(0x77, final_size);
    }
    static void update_max_stream_data(QuicStreamRecvQueue &queue, std::uint64_t limit) noexcept {
        queue.update_max_stream_data(limit);
    }
};

} // namespace fiber::quic

namespace {

using namespace fiber::quic;
using fiber::fuzz::FuzzInput;
using Access = QuicStreamRecvQueueTestAccess;

constexpr std::uint64_t kFarOffset = (1ULL << 62U) - 0x10000;

// The byte a peer sends at `offset`; `variant` != 0 models a retransmission
// that (illegally) carries different bytes for the same offsets.
std::uint8_t wire_byte(std::uint64_t offset, std::uint8_t variant) {
    std::uint64_t x = offset * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29U;
    return static_cast<std::uint8_t>(x ^ (variant * 0x5bU));
}

// Received byte ranges [start, end) with the variant that won each byte.
class Model {
public:
    struct Extent {
        std::uint64_t end = 0;
        std::uint8_t variant = 0;
    };

    // Fills only the holes of [start, end) at or past `next_`.
    void insert(std::uint64_t start, std::uint64_t end, std::uint8_t variant) {
        start = std::max(start, next_);
        while (start < end) {
            auto it = extents_.upper_bound(start);
            if (it != extents_.begin()) {
                auto prev = std::prev(it);
                if (prev->second.end > start) {
                    start = prev->second.end;
                    continue;
                }
            }
            const std::uint64_t hole_end = it != extents_.end() ? std::min(end, it->first) : end;
            extents_.emplace(start, Extent{hole_end, variant});
            start = hole_end;
        }
    }

    // The variant of the byte at `next_`, or -1 when it has not arrived.
    [[nodiscard]] int next_variant() const {
        auto it = extents_.upper_bound(next_);
        if (it == extents_.begin()) {
            return -1;
        }
        --it;
        return it->second.end > next_ ? it->second.variant : -1;
    }

    void advance(std::uint64_t n) {
        next_ += n;
        while (!extents_.empty() && extents_.begin()->second.end <= next_) {
            extents_.erase(extents_.begin());
        }
    }

    void discard() { extents_.clear(); }

    [[nodiscard]] std::uint64_t next() const { return next_; }
    [[nodiscard]] bool has_buffered() const { return !extents_.empty(); }

private:
    std::map<std::uint64_t, Extent> extents_;
    std::uint64_t next_ = 0;
};

struct Config {
    bool stream = false;
    bool budget = false;
    bool sliced = false;
    std::size_t buffer_limit = 0;
    std::size_t max_extents = 0;
};

fiber::mem::IoBuf make_data(const Config &config, std::uint64_t offset, std::size_t len, std::uint8_t variant,
                            std::size_t slack) {
    if (len == 0) {
        return {};
    }
    const std::size_t head = config.sliced ? slack : 0;
    const std::size_t cap = config.sliced ? head + len + slack * 3 : len;
    fiber::mem::IoBuf buf =
            config.budget ? fiber::mem::IoBuf::allocate_trackable(cap) : fiber::mem::IoBuf::allocate(cap);
    FIBER_ASSERT(buf.valid());
    std::uint8_t *dst = buf.writable_data();
    std::memset(dst, 0xEE, cap);
    for (std::size_t i = 0; i < len; ++i) {
        dst[head + i] = wire_byte(offset + i, variant);
    }
    buf.commit(head + len);
    if (head == 0) {
        return buf;
    }
    return buf.retain_slice(head, len);
}

// Verifies a delivered chain against the model and advances it.
std::size_t check_delivered(Model &model, const fiber::mem::IoBufChain &out) {
    std::size_t total = 0;
    for (const fiber::mem::IoBufNode *node = out.front_node(); node != nullptr; node = node->next) {
        const std::uint8_t *bytes = node->buf.readable_data();
        for (std::size_t i = 0; i < node->buf.readable(); ++i) {
            const int variant = model.next_variant();
            FIBER_FUZZ_CHECK(variant >= 0);
            FIBER_FUZZ_CHECK(bytes[i] == wire_byte(model.next(), static_cast<std::uint8_t>(variant)));
            model.advance(1);
        }
        total += node->buf.readable();
    }
    FIBER_FUZZ_CHECK(total == out.readable_bytes());
    return total;
}

struct DataOp {
    std::uint64_t offset = 0;
    std::size_t len = 0;
    std::uint8_t variant = 0;
    bool fin = false;
};

// Extra backing around a sliced payload: from none up to a datagram's worth,
// so the budget's amplification check (and compaction) is reachable.
std::size_t slack_of(std::uint8_t kind) { return static_cast<std::size_t>(kind >> 6) * 1500; }

DataOp read_data_op(std::uint8_t kind, FuzzInput &in) {
    DataOp op{};
    op.offset = in.u16();
    op.len = in.u16() & 0x1fff;
    if ((kind & 0x04) != 0) {
        op.offset += static_cast<std::uint64_t>(in.u16()) << 16U;
    }
    if ((kind & 0x08) != 0) {
        op.offset += kFarOffset;
    }
    op.variant = (kind & 0x10) != 0 ? 1 : 0;
    op.fin = (kind & 0x20) != 0;
    return op;
}

void run_reassembler(const Config &config, FuzzInput &in) {
    fiber::mem::IoBufStorageBudget budget(config.buffer_limit * 8 + 4096);
    {
        QuicDataReassembler reassembler(fiber::fuzz::quic_loop().io_buf_node_pool(),
                                        {.buffer_limit = config.buffer_limit,
                                         .max_active_extents = config.max_extents,
                                         .storage_budget = config.budget ? &budget : nullptr});
        Model model;
        for (int ops = 0; !in.empty() && ops < 256; ++ops) {
            const std::uint8_t kind = in.u8();
            switch (kind & 0x03) {
                case 0: {
                    const DataOp op = read_data_op(kind, in);
                    auto inserted = reassembler.insert(
                            op.offset, make_data(config, op.offset, op.len, op.variant, slack_of(kind)));
                    if (inserted) {
                        model.insert(op.offset, op.offset + op.len, op.variant);
                    } else {
                        FIBER_FUZZ_CHECK(inserted.error() == fiber::common::IoErr::MessageTooLarge ||
                                         inserted.error() == fiber::common::IoErr::NoMem);
                    }
                    break;
                }
                case 1: {
                    const std::size_t max = in.u16();
                    fiber::mem::IoBufChain out;
                    auto taken = reassembler.take_contiguous(out, max == 0 ? SIZE_MAX : max);
                    FIBER_FUZZ_CHECK(taken.has_value());
                    FIBER_FUZZ_CHECK(check_delivered(model, out) == *taken);
                    if (max != 0) {
                        FIBER_FUZZ_CHECK(*taken <= max);
                    }
                    if (*taken == 0) {
                        FIBER_FUZZ_CHECK(model.next_variant() < 0);
                    }
                    break;
                }
                case 2:
                    reassembler.discard_buffered();
                    model.discard();
                    break;
                default:
                    break;
            }
            FIBER_FUZZ_CHECK(reassembler.next_offset() == model.next());
            FIBER_FUZZ_CHECK(reassembler.has_contiguous_data() == (model.next_variant() >= 0));
            if (!model.has_buffered()) {
                FIBER_FUZZ_CHECK(reassembler.buffered_bytes() == 0 && reassembler.active_extent_count() == 0);
            }
        }
    }
    FIBER_FUZZ_CHECK(budget.retained_capacity() == 0);
}

void run_stream(const Config &config, FuzzInput &in) {
    fiber::mem::IoBufStorageBudget budget(config.buffer_limit * 8 + 4096);
    {
        QuicStreamRecvQueue queue(fiber::fuzz::quic_loop().io_buf_node_pool(),
                                  {.buffer_limit = config.buffer_limit,
                                   .low_water = config.buffer_limit / 4,
                                   .max_stream_data = config.buffer_limit,
                                   .storage_budget = config.budget ? &budget : nullptr});
        Model model;
        std::uint64_t max_stream_data = config.buffer_limit;
        std::uint64_t received_end = 0;
        bool has_final = false;
        std::uint64_t final_size = 0;
        bool terminal = false; // RESET_STREAM received or STOP_SENDING sent

        for (int ops = 0; !in.empty() && ops < 256; ++ops) {
            const std::uint8_t kind = in.u8();
            switch (kind & 0x03) {
                case 0: {
                    const DataOp op = read_data_op(kind, in);
                    const std::uint64_t end = op.offset + op.len;
                    // RFC 9000 4.5: the final size can neither move nor be
                    // exceeded, and flow control bounds every offset.
                    const bool must_fail = end > max_stream_data || (has_final && end > final_size) ||
                                           (op.fin && has_final && end != final_size) || (op.fin && end < received_end);
                    auto received = Access::recv(
                            queue, op.offset, make_data(config, op.offset, op.len, op.variant, slack_of(kind)), op.fin);
                    if (must_fail) {
                        FIBER_FUZZ_CHECK(!received.has_value());
                        // A connection closes on any of these errors.
                        return;
                    }
                    if (!received) {
                        // Only the buffer bounds may refuse in-limit data.
                        FIBER_FUZZ_CHECK(!terminal);
                        FIBER_FUZZ_CHECK(received.error() == fiber::common::IoErr::MessageTooLarge ||
                                         received.error() == fiber::common::IoErr::NoMem);
                        return;
                    }
                    received_end = std::max(received_end, end);
                    if (op.fin) {
                        has_final = true;
                        final_size = end;
                    }
                    if (!terminal) {
                        model.insert(op.offset, end, op.variant);
                    } else {
                        FIBER_FUZZ_CHECK(*received == 0);
                    }
                    FIBER_FUZZ_CHECK(queue.received_end_offset() == received_end);
                    FIBER_FUZZ_CHECK(queue.has_final_size() == has_final);
                    break;
                }
                case 1: {
                    const std::size_t max = in.u16();
                    fiber::mem::IoBufChain out;
                    auto taken = queue.try_take(max == 0 ? SIZE_MAX : max, out);
                    if (terminal) {
                        FIBER_FUZZ_CHECK(!taken.has_value());
                        break;
                    }
                    if (!taken) {
                        FIBER_FUZZ_CHECK(taken.error() == fiber::common::IoErr::WouldBlock);
                        FIBER_FUZZ_CHECK(model.next_variant() < 0);
                        FIBER_FUZZ_CHECK(!(has_final && model.next() == final_size));
                        break;
                    }
                    FIBER_FUZZ_CHECK(check_delivered(model, out) == *taken);
                    FIBER_FUZZ_CHECK(out.complete() == (has_final && model.next() == final_size));
                    if (has_final) {
                        FIBER_FUZZ_CHECK(model.next() <= final_size);
                    }
                    break;
                }
                case 2:
                    if ((kind & 0x04) != 0) {
                        const std::uint64_t final_reset = in.u16();
                        const bool must_fail = received_end > final_reset || (has_final && final_reset != final_size);
                        auto reset = Access::reset(queue, final_reset);
                        if (must_fail) {
                            FIBER_FUZZ_CHECK(!reset.has_value());
                            return;
                        }
                        // A RESET_STREAM within the rules is always accepted.
                        FIBER_FUZZ_CHECK(reset.has_value());
                        has_final = true;
                        final_size = final_reset;
                        received_end = final_reset;
                    } else {
                        queue.stop_receiving(0x42);
                    }
                    terminal = true;
                    model.discard();
                    FIBER_FUZZ_CHECK(queue.buffered_bytes() == 0);
                    break;
                default: {
                    const std::uint64_t limit = max_stream_data + (static_cast<std::uint64_t>(in.u16()) << 4U);
                    Access::update_max_stream_data(queue, limit);
                    max_stream_data = std::max(max_stream_data, limit);
                    FIBER_FUZZ_CHECK(queue.max_stream_data() == max_stream_data);
                    break;
                }
            }
            FIBER_FUZZ_CHECK(queue.next_read_offset() == model.next());
            if (!model.has_buffered()) {
                FIBER_FUZZ_CHECK(queue.buffered_bytes() == 0 && queue.active_extent_count() == 0);
            }
            (void) queue.should_extend_max_stream_data();
            (void) queue.next_max_stream_data_limit();
        }
    }
    FIBER_FUZZ_CHECK(budget.retained_capacity() == 0);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    FuzzInput in(data, size);
    const std::uint8_t config_byte = in.u8();
    static constexpr std::size_t kLimits[] = {64 * 1024, 4096, 512, 64};
    static constexpr std::size_t kExtents[] = {kQuicCryptoRecvMaxActiveExtents, 16, 4, 1};
    Config config{};
    config.stream = (config_byte & 0x01) != 0;
    config.budget = (config_byte & 0x02) != 0;
    config.buffer_limit = kLimits[(config_byte >> 2) & 0x03];
    config.max_extents = kExtents[(config_byte >> 4) & 0x03];
    config.sliced = (config_byte & 0x40) != 0;
    fiber::fuzz::run_in_quic_loop([&] {
        if (config.stream) {
            run_stream(config, in);
        } else {
            run_reassembler(config, in);
        }
    });
    return 0;
}
