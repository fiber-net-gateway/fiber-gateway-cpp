#ifndef FIBER_REGEX_PROGRAM_H
#define FIBER_REGEX_PROGRAM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::regex::detail {

enum class InstOp : std::uint8_t {
    Alt = 0, // choose between out and out1
    AltMatch, // Alt where one branch loops over [00-FF] and the other matches
    ByteRange, // the next byte, possibly case-folded, must be in lo-hi
    Capture, // record the position in capture register cap
    EmptyWidth, // empty-width assertion; the bits are in empty()
    Match, // found a match
    Nop, // no-op, occasionally unavoidable
    Fail, // never matches, occasionally unavoidable
};
inline constexpr std::size_t kNumInstOps = 8;

enum EmptyOp : std::uint32_t {
    kEmptyBeginLine = 1U << 0U, // ^
    kEmptyEndLine = 1U << 1U, // $
    kEmptyBeginText = 1U << 2U, // \A
    kEmptyEndText = 1U << 3U, // \z
    kEmptyWordBoundary = 1U << 4U, // \b
    kEmptyNonWordBoundary = 1U << 5U, // \B
};

class Program;

// One instruction, packed into 8 bytes as in RE2. out_opcode_ holds 28 bits of
// out, 1 bit of last and 3 bits of opcode. arg_ is out1, cap, match id or the
// empty flags, or for ByteRange lo (8 bits), hi (8) and hint-and-foldcase (16).
class Inst {
public:
    static constexpr std::uint32_t kMaxInst = (1U << 28U) - 1U;

    void init_alt(std::uint32_t out, std::uint32_t out1) noexcept {
        set_out_opcode(out, InstOp::Alt);
        arg_ = out1;
    }
    void init_byte_range(std::uint32_t lo, std::uint32_t hi, bool foldcase, std::uint32_t out) noexcept {
        set_out_opcode(out, InstOp::ByteRange);
        arg_ = (lo & 0xFFU) | ((hi & 0xFFU) << 8U) | (foldcase ? 1U << 16U : 0U);
    }
    void init_capture(std::uint32_t cap, std::uint32_t out) noexcept {
        set_out_opcode(out, InstOp::Capture);
        arg_ = cap;
    }
    void init_empty_width(std::uint32_t empty, std::uint32_t out) noexcept {
        set_out_opcode(out, InstOp::EmptyWidth);
        arg_ = empty;
    }
    void init_match(std::uint32_t id) noexcept {
        set_opcode(InstOp::Match);
        arg_ = id;
    }
    void init_nop(std::uint32_t out) noexcept { set_out_opcode(out, InstOp::Nop); }
    void init_fail() noexcept { set_opcode(InstOp::Fail); }
    void clear() noexcept {
        out_opcode_ = 0;
        arg_ = 0;
    }

    [[nodiscard]] InstOp opcode() const noexcept { return static_cast<InstOp>(out_opcode_ & 7U); }
    [[nodiscard]] bool last() const noexcept { return ((out_opcode_ >> 3U) & 1U) != 0; }
    [[nodiscard]] std::uint32_t out() const noexcept { return out_opcode_ >> 4U; }
    [[nodiscard]] std::uint32_t out1() const noexcept { return arg_; }
    [[nodiscard]] std::uint32_t cap() const noexcept { return arg_; }
    [[nodiscard]] std::uint32_t lo() const noexcept { return arg_ & 0xFFU; }
    [[nodiscard]] std::uint32_t hi() const noexcept { return (arg_ >> 8U) & 0xFFU; }
    [[nodiscard]] bool foldcase() const noexcept { return ((arg_ >> 16U) & 1U) != 0; }
    // Delta to the next instruction in the current list worth exploring if this
    // one matched. 0 means nothing else can match the same byte.
    [[nodiscard]] std::uint32_t hint() const noexcept { return arg_ >> 17U; }
    [[nodiscard]] std::uint32_t match_id() const noexcept { return arg_; }
    [[nodiscard]] std::uint32_t empty() const noexcept { return arg_; }

    // Whether this ByteRange matches c. A c of -1 means end of text.
    [[nodiscard]] bool matches(int c) const noexcept {
        if (foldcase() && c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        return std::cmp_less_equal(lo(), c) && std::cmp_less_equal(c, hi());
    }

    // Whether this AltMatch prefers to keep consuming bytes.
    [[nodiscard]] bool greedy(const Program &program) const noexcept;

    void set_opcode(InstOp opcode) noexcept {
        out_opcode_ = (out() << 4U) | (last() ? 8U : 0U) | static_cast<std::uint32_t>(opcode);
    }
    void set_last() noexcept { out_opcode_ |= 8U; }
    void set_out(std::uint32_t out) noexcept { out_opcode_ = (out << 4U) | (out_opcode_ & 15U); }
    void set_out1(std::uint32_t out1) noexcept { arg_ = out1; }
    void set_hint(std::uint32_t hint) noexcept { arg_ |= hint << 17U; }

private:
    void set_out_opcode(std::uint32_t out, InstOp opcode) noexcept {
        out_opcode_ = (out << 4U) | (last() ? 8U : 0U) | static_cast<std::uint32_t>(opcode);
    }

    std::uint32_t out_opcode_ = 0;
    std::uint32_t arg_ = 0;
};

static_assert(sizeof(Inst) == 8);

[[nodiscard]] constexpr bool is_word_char(std::uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Compiled form of a regexp. Instruction 0 is always Fail. After flatten() the
// instructions form lists: an entry jumps to a list and every instruction up to
// the one marked last() is tried in priority order.
class Program {
public:
    Program() = default;

    [[nodiscard]] const Inst &inst(std::uint32_t id) const noexcept { return inst_[id]; }
    [[nodiscard]] std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(inst_.size()); }
    [[nodiscard]] std::uint32_t start() const noexcept { return start_; }
    [[nodiscard]] std::uint32_t start_unanchored() const noexcept { return start_unanchored_; }
    [[nodiscard]] std::uint32_t list_count() const noexcept { return list_count_; }
    [[nodiscard]] std::uint32_t inst_count(InstOp op) const noexcept {
        return inst_count_[static_cast<std::size_t>(op)];
    }
    // Unique for the life of the process, unlike the program's address.
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] bool reversed() const noexcept { return reversed_; }
    [[nodiscard]] bool anchor_start() const noexcept { return anchor_start_; }
    [[nodiscard]] bool anchor_end() const noexcept { return anchor_end_; }

    // First byte of a required literal prefix, or -1. Lets an unanchored search
    // skip with memchr while no thread is alive.
    [[nodiscard]] int prefix_byte() const noexcept { return prefix_byte_; }

    // Bytes that no instruction can tell apart share a class.
    [[nodiscard]] const std::array<std::uint8_t, 256> &bytemap() const noexcept { return bytemap_; }
    [[nodiscard]] std::uint32_t bytemap_range() const noexcept { return bytemap_range_; }

    // BitState needs a list number for each instruction, which is only kept for
    // small programs. Its visited bitmap in turn bounds the text it accepts.
    [[nodiscard]] bool can_bit_state() const noexcept { return !list_heads_.empty(); }
    [[nodiscard]] std::size_t bit_state_text_max_size() const noexcept { return bit_state_text_max_size_; }
    [[nodiscard]] const std::vector<std::uint16_t> &list_heads() const noexcept { return list_heads_; }

    // Whether at every input byte only one thread can proceed. Such programs have
    // an equivalent DFA of about their own size, built by build_one_pass().
    [[nodiscard]] bool is_one_pass() const noexcept { return !one_pass_nodes_.empty(); }
    [[nodiscard]] const std::vector<std::uint32_t> &one_pass_nodes() const noexcept { return one_pass_nodes_; }

    // Empty-width flags that hold at position `pos` of `text`.
    [[nodiscard]] static std::uint32_t empty_flags(std::string_view text, std::size_t pos) noexcept;

private:
    friend class Compiler;

    // Bypasses Nops and marks .* loops that end in a match as AltMatch.
    void optimize();
    // Rewrites the instructions into lists, which is what the engines execute.
    void flatten();
    void compute_byte_map();
    void build_one_pass();

    std::vector<Inst> inst_{};
    std::uint32_t start_ = 0;
    std::uint32_t start_unanchored_ = 0;
    std::uint32_t list_count_ = 0;
    std::array<std::uint32_t, kNumInstOps> inst_count_{};
    std::array<std::uint8_t, 256> bytemap_{};
    std::uint32_t bytemap_range_ = 0;
    std::vector<std::uint16_t> list_heads_{};
    std::size_t bit_state_text_max_size_ = 0;
    std::vector<std::uint32_t> one_pass_nodes_{};
    std::uint64_t id_ = 0;
    int prefix_byte_ = -1;
    bool reversed_ = false;
    bool anchor_start_ = false;
    bool anchor_end_ = false;
};

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_PROGRAM_H
