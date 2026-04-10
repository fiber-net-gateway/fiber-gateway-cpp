#ifndef FIBER_LITE_NGINX_CONFIG_AST_H
#define FIBER_LITE_NGINX_CONFIG_AST_H

#include <cstddef>
#include <string>
#include <vector>

namespace fiber::lite_nginx::config {

struct SourceLocation {
    std::string source_name;
    std::size_t line = 1;
    std::size_t column = 1;
    std::size_t offset = 0;
};

struct ConfigError {
    std::string message;
    SourceLocation location;
};

enum class TokenKind : unsigned char {
    Word,
    String,
    Semicolon,
    LBrace,
    RBrace,
    Equal,
    End,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    SourceLocation location;
};

struct DirectiveNode {
    std::string name;
    std::vector<std::string> args;
    std::vector<DirectiveNode> children;
    SourceLocation location;
    bool has_block = false;
};

struct Document {
    std::vector<DirectiveNode> directives;
};

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_AST_H
