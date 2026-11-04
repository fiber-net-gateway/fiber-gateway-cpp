// Benchmarks fiber::regex on the OWASP CRS rules in tests/support/third_party:
//
//   build/example/regex_crs_bench tests/support/third_party/crs_bd1c42a
//
// It compares running every rule on its own with one RegexSet per group of rules that
// see the same text, and with one RegexSet of everything. Each pass runs all of the
// rules on all of the payloads, which no real request does, so the absolute numbers
// mean little: compare them with each other.
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/regex/Regex.h>
#include <fiber/regex/RegexSet.h>
#include <fiber/regex/Scratch.h>

namespace {

using namespace fiber::regex;
using Clock = std::chrono::steady_clock;

struct Rule {
    std::string location;
    std::string pattern;
};

std::string from_hex(std::string_view hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned value = 0;
        std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
        out.push_back(static_cast<char>(value));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> read_tsv(const std::filesystem::path &path) {
    std::vector<std::pair<std::string, std::string>> rows;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t tab = line.find('\t');
        rows.emplace_back(line.substr(0, tab), tab == std::string::npos ? "" : line.substr(tab + 1));
    }
    return rows;
}

void measure(const char *name, std::size_t text_bytes, Scratch &scratch, const std::function<void()> &pass) {
    pass(); // warm the DFA caches
    const auto start = Clock::now();
    pass();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("%-22s %9.1f ms %8.2f MB/s %8.2f MiB\n", name, seconds * 1e3,
                static_cast<double>(text_bytes) / seconds / 1e6,
                static_cast<double>(scratch.dfa_bytes_used()) / 1048576.0);
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <crs fixture directory>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path dir(argv[1]);

    std::vector<Rule> rules;
    for (auto &[location, pattern]: read_tsv(dir / "rx_patterns.txt")) {
        rules.push_back(Rule{.location = std::move(location), .pattern = std::move(pattern)});
    }
    std::map<std::string, std::string> group_of;
    for (auto &[location, group]: read_tsv(dir / "rule_groups.txt")) {
        group_of.emplace(std::move(location), std::move(group));
    }
    std::vector<std::string> texts;
    std::size_t text_bytes = 0;
    for (const auto &[payload, hits]: read_tsv(dir / "payloads.txt")) {
        texts.push_back(from_hex(payload));
        text_bytes += texts.back().size();
    }

    Options options;
    options.encoding = Encoding::Bytes;
    options.dot_matches_newline = true;

    std::vector<Regex> regexes;
    regexes.reserve(rules.size());
    std::map<std::string, RegexSet::Builder> builders;
    RegexSet::Builder everything(options);
    for (const Rule &rule: rules) {
        auto regex = Regex::compile(rule.pattern, options);
        if (!regex) {
            std::fprintf(stderr, "%s does not compile\n", rule.location.c_str());
            return 1;
        }
        regexes.push_back(std::move(*regex));
        const auto group = group_of.find(rule.location);
        auto &builder =
                builders.try_emplace(group == group_of.end() ? rule.location : group->second, options).first->second;
        (void) builder.add(rule.pattern);
        (void) everything.add(rule.pattern);
    }
    std::vector<RegexSet> sets;
    sets.reserve(builders.size());
    for (auto &[group, builder]: builders) {
        sets.push_back(std::move(*std::move(builder).build()));
    }
    const RegexSet all = std::move(*std::move(everything).build());

    std::printf("%zu rules in %zu groups, %zu payloads, %zu bytes\n\n", rules.size(), sets.size(), texts.size(),
                text_bytes);
    std::printf("%-22s %12s %13s %12s\n", "", "warm pass", "of input", "DFA memory");

    {
        Scratch scratch;
        measure("rule by rule", text_bytes, scratch, [&] {
            for (const std::string &text: texts) {
                for (const Regex &regex: regexes) {
                    (void) regex.test(text, scratch);
                }
            }
        });
    }
    {
        Scratch scratch;
        std::vector<MatchSet> hits;
        hits.reserve(sets.size());
        for (const RegexSet &set: sets) {
            hits.push_back(set.make_match_set());
        }
        measure("set per group", text_bytes, scratch, [&] {
            for (const std::string &text: texts) {
                for (std::size_t i = 0; i < sets.size(); ++i) {
                    (void) sets[i].scan(text, hits[i], scratch);
                }
            }
        });
    }
    {
        Scratch scratch;
        MatchSet hits = all.make_match_set();
        measure("one set of everything", text_bytes, scratch, [&] {
            for (const std::string &text: texts) {
                (void) all.scan(text, hits, scratch);
            }
        });
    }
    return 0;
}
