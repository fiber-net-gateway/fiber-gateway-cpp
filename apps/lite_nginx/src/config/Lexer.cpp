#include "Lexer.h"

#include <cctype>

namespace fiber::lite_nginx::config {
namespace {

bool is_whitespace(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

bool is_delimiter(char ch) {
    return is_whitespace(ch) || ch == '#' || ch == '{' || ch == '}' || ch == ';' || ch == '=' || ch == '"' ||
           ch == '\'';
}

char decode_escape(char ch) {
    switch (ch) {
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        default:
            return ch;
    }
}

} // namespace

Lexer::Lexer(std::string_view input, std::string source_name) : input_(input), source_name_(std::move(source_name)) {}

std::expected<std::vector<Token>, ConfigError> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (!eof()) {
        skip_whitespace();
        if (eof()) {
            break;
        }

        if (peek() == '#') {
            skip_comment();
            continue;
        }

        if (peek() == '{') {
            tokens.push_back(make_simple_token(TokenKind::LBrace));
            advance();
            continue;
        }
        if (peek() == '}') {
            tokens.push_back(make_simple_token(TokenKind::RBrace));
            advance();
            continue;
        }
        if (peek() == ';') {
            tokens.push_back(make_simple_token(TokenKind::Semicolon));
            advance();
            continue;
        }
        if (peek() == '=') {
            tokens.push_back(make_simple_token(TokenKind::Equal));
            advance();
            continue;
        }
        if (peek() == '"' || peek() == '\'') {
            auto string_result = read_string();
            if (!string_result) {
                return std::unexpected(string_result.error());
            }
            tokens.push_back(std::move(*string_result));
            continue;
        }

        auto word_result = read_word();
        if (!word_result) {
            return std::unexpected(word_result.error());
        }
        tokens.push_back(std::move(*word_result));
    }

    Token end_token;
    end_token.kind = TokenKind::End;
    end_token.location = current_location();
    tokens.push_back(std::move(end_token));
    return tokens;
}

bool Lexer::eof() const noexcept { return pos_ >= input_.size(); }

char Lexer::peek(std::size_t offset) const noexcept {
    if (pos_ + offset >= input_.size()) {
        return '\0';
    }
    return input_[pos_ + offset];
}

SourceLocation Lexer::current_location() const {
    return SourceLocation{
            .source_name = source_name_,
            .line = line_,
            .column = column_,
            .offset = pos_,
    };
}

void Lexer::advance() {
    if (eof()) {
        return;
    }
    if (input_[pos_] == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    ++pos_;
}

void Lexer::skip_whitespace() {
    while (!eof() && is_whitespace(peek())) {
        advance();
    }
}

void Lexer::skip_comment() {
    while (!eof() && peek() != '\n') {
        advance();
    }
}

std::expected<Token, ConfigError> Lexer::read_word() {
    Token token;
    token.kind = TokenKind::Word;
    token.location = current_location();

    while (!eof() && !is_delimiter(peek())) {
        token.text.push_back(peek());
        advance();
    }

    if (token.text.empty()) {
        return std::unexpected(ConfigError{
                .message = "unexpected character in config",
                .location = token.location,
        });
    }
    return token;
}

std::expected<Token, ConfigError> Lexer::read_string() {
    Token token;
    token.kind = TokenKind::String;
    token.location = current_location();

    const char quote = peek();
    advance();

    while (!eof()) {
        char ch = peek();
        if (ch == quote) {
            advance();
            return token;
        }
        if (ch == '\\') {
            advance();
            if (eof()) {
                return std::unexpected(ConfigError{
                        .message = "unterminated escape sequence in string literal",
                        .location = current_location(),
                });
            }
            token.text.push_back(decode_escape(peek()));
            advance();
            continue;
        }
        token.text.push_back(ch);
        advance();
    }

    return std::unexpected(ConfigError{
            .message = "unterminated string literal",
            .location = token.location,
    });
}

Token Lexer::make_simple_token(TokenKind kind) {
    Token token;
    token.kind = kind;
    token.location = current_location();
    return token;
}

} // namespace fiber::lite_nginx::config
