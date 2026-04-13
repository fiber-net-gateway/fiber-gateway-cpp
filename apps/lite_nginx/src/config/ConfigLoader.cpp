#include "ConfigLoader.h"

#include <fstream>
#include <sstream>

#include "Lexer.h"
#include "Parser.h"
#include "Semantic.h"

namespace fiber::lite_nginx::config {

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

    SemanticAnalyzer analyzer;
    return analyzer.analyze(*document);
}

std::expected<MainConfig, ConfigError> ConfigLoader::load_from_file(const std::string &path) {
    std::ifstream input(path);
    if (!input) {
        return std::unexpected(ConfigError{
                .message = "failed to open config file",
                .location =
                        SourceLocation{
                                .source_name = path,
                                .line = 1,
                                .column = 1,
                                .offset = 0,
                        },
        });
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return load_from_string(buffer.str(), path);
}

} // namespace fiber::lite_nginx::config
