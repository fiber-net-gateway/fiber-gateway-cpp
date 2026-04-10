#ifndef FIBER_LITE_NGINX_CONFIG_PARSER_H
#define FIBER_LITE_NGINX_CONFIG_PARSER_H

#include <expected>
#include <vector>

#include "Ast.h"

namespace fiber::lite_nginx::config {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    [[nodiscard]] std::expected<Document, ConfigError> parse();

private:
    [[nodiscard]] const Token &peek() const noexcept;
    [[nodiscard]] const Token &next() noexcept;
    [[nodiscard]] bool match(TokenKind kind) noexcept;
    [[nodiscard]] std::expected<DirectiveNode, ConfigError> parse_directive();
    [[nodiscard]] std::expected<std::string, ConfigError> parse_argument();
    [[nodiscard]] ConfigError make_error(const char *message, const Token &token) const;

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_PARSER_H
