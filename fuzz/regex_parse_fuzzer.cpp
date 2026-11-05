// Feeds arbitrary patterns through the whole compile pipeline, and then matches
// the compiled program against the rest of the input. The first byte selects the
// options; the pattern and text are split at the first 0xFF 0x00 pair.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <fiber/regex/Regex.h>
#include <fiber/regex/Scratch.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    using namespace fiber::regex;

    if (size < 1) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    const std::string_view input(reinterpret_cast<const char *>(data + 1), size - 1);

    Options options;
    options.encoding = (selector & 1U) != 0 ? Encoding::Bytes : Encoding::Utf8;
    options.ignore_case = (selector & 2U) != 0;
    options.dot_matches_newline = (selector & 4U) != 0;
    options.multi_line = (selector & 8U) != 0;

    const std::size_t split = input.find("\xff\x00", 0, 2);
    const std::string_view pattern = input.substr(0, split);
    const std::string_view text = split == std::string_view::npos ? std::string_view{} : input.substr(split + 2);

    auto regex = Regex::compile(pattern, options);
    if (!regex) {
        return 0;
    }

    Scratch scratch;
    // Keeps pathological programs from timing the fuzzer out.
    WorkBudget budget{1U << 22U};
    (void) regex->test(text, scratch, budget);

    std::array<Span, 8> spans;
    (void) regex->find(text, 0, spans, scratch, budget);
    return 0;
}
