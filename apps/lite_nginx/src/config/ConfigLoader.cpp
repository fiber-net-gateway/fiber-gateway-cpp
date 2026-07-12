#include "ConfigLoader.h"

#include <fstream>
#include <sstream>
#include <utility>

#include "Lexer.h"
#include "Parser.h"
#include "PathResolve.h"
#include "Semantic.h"

namespace fiber::lite_nginx::config {
namespace {

bool contains_variable(std::string_view value) { return value.find('$') != std::string_view::npos; }

ConfigError make_error(const SourceLocation &location, std::string message) {
    return ConfigError{
            .message = std::move(message),
            .location = location,
    };
}

} // namespace

std::expected<Document, ConfigError> ConfigLoader::parse_document(std::string_view input, std::string source_name) {
    Lexer lexer(input, std::move(source_name));
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return std::unexpected(tokens.error());
    }

    Parser parser(std::move(*tokens));
    return parser.parse();
}

std::expected<MainConfig, ConfigError> ConfigLoader::load_from_string(std::string_view input, std::string source_name) {
    auto document = parse_document(input, std::move(source_name));
    if (!document) {
        return std::unexpected(document.error());
    }

    // Expand `include` directives before semantic analysis: each included file's
    // directives are spliced in place of the include node, carrying the included
    // file's canonical path as their source_name so nested includes and path
    // directives (script_file/certificate/...) resolve relative to that file.
    auto expand_result = expand_includes(document->directives, {});
    if (!expand_result) {
        return std::unexpected(expand_result.error());
    }

    SemanticAnalyzer analyzer;
    return analyzer.analyze(*document);
}

std::expected<MainConfig, ConfigError> ConfigLoader::load_from_file(const std::string &path) {
    // Anchor the config to its absolute, symlink-resolved location so every relative
    // path inside it resolves against the config's real directory, not the process pwd
    // (a relative `--config` argument would otherwise make downstream paths pwd-relative).
    const std::string abs = canonicalize_path(path);
    std::ifstream input(abs, std::ios::binary);
    if (!input) {
        return std::unexpected(ConfigError{
                .message = "failed to open config file",
                .location =
                        SourceLocation{
                                .source_name = abs,
                                .line = 1,
                                .column = 1,
                                .offset = 0,
                        },
        });
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return load_from_string(buffer.str(), abs);
}

std::expected<Document, ConfigError> ConfigLoader::load_include_document(const std::string &canonical_path) {
    std::ifstream input(canonical_path, std::ios::binary);
    if (!input) {
        return std::unexpected(ConfigError{
                .message = "include file not found: " + canonical_path,
                .location =
                        SourceLocation{
                                .source_name = canonical_path,
                                .line = 1,
                                .column = 1,
                                .offset = 0,
                        },
        });
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse_document(buffer.str(), canonical_path);
}

std::expected<void, ConfigError> ConfigLoader::expand_includes(std::vector<DirectiveNode> &directives,
                                                               std::vector<std::string> ancestors) {
    std::vector<DirectiveNode> result;
    result.reserve(directives.size());

    for (auto &directive: directives) {
        if (directive.name != "include") {
            // Recurse into block children: an `include` inside a block is expanded in
            // place before the semantic analyzer sees the block. The ancestor stack is
            // unchanged (same file as the parent directive).
            if (directive.has_block) {
                auto child_result = expand_includes(directive.children, ancestors);
                if (!child_result) {
                    return std::unexpected(child_result.error());
                }
            }
            result.push_back(std::move(directive));
            continue;
        }

        if (directive.has_block) {
            return std::unexpected(make_error(directive.location, "include must not be a block"));
        }
        if (directive.args.size() != 1) {
            return std::unexpected(make_error(directive.location, "include expects exactly one path argument"));
        }
        const std::string &arg = directive.args[0];
        if (arg.empty() || contains_variable(arg)) {
            return std::unexpected(make_error(directive.location, "include path must be a non-empty static path"));
        }

        // Resolve the target relative to the file that contains this `include`, then
        // canonicalize it so the cycle key and the spliced directives' source_name are
        // stable across symlinks/relative variations.
        const std::string target = resolve_config_path(directive.location.source_name, arg);
        const std::string canonical = canonicalize_path(target);

        for (const auto &ancestor: ancestors) {
            if (ancestor == canonical) {
                return std::unexpected(make_error(directive.location, "include cycle detected: " + canonical));
            }
        }

        auto sub_document = load_include_document(canonical);
        if (!sub_document) {
            return std::unexpected(sub_document.error());
        }

        auto sub_ancestors = ancestors;
        sub_ancestors.push_back(canonical);
        auto expand_result = expand_includes(sub_document->directives, std::move(sub_ancestors));
        if (!expand_result) {
            return std::unexpected(expand_result.error());
        }

        // Splice the included file's top-level directives in place of the `include` node.
        // Their source_name is already `canonical`, so any further path resolution in
        // them anchors to the included file.
        for (auto &sub_directive: sub_document->directives) {
            result.push_back(std::move(sub_directive));
        }
    }

    directives = std::move(result);
    return {};
}

} // namespace fiber::lite_nginx::config
