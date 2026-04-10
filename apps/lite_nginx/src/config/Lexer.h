#ifndef FIBER_LITE_NGINX_CONFIG_LEXER_H
#define FIBER_LITE_NGINX_CONFIG_LEXER_H

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "Ast.h"

namespace fiber::lite_nginx::config {

class Lexer {
public:
    Lexer(std::string_view input, std::string source_name);

    [[nodiscard]] std::expected<std::vector<Token>, ConfigError> tokenize();

private:
    [[nodiscard]] bool eof() const noexcept;
    [[nodiscard]] char peek(std::size_t offset = 0) const noexcept;
    [[nodiscard]] SourceLocation current_location() const;
    void advance();
    void skip_whitespace();
    void skip_comment();
    [[nodiscard]] std::expected<Token, ConfigError> read_word();
    [[nodiscard]] std::expected<Token, ConfigError> read_string();
    Token make_simple_token(TokenKind kind);

    std::string_view input_;
    std::string source_name_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_LEXER_H
