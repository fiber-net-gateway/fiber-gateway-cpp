#include "Parser.h"

namespace fiber::lite_nginx::config {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::expected<Document, ConfigError> Parser::parse() {
    Document document;
    while (peek().kind != TokenKind::End) {
        if (peek().kind == TokenKind::RBrace) {
            return std::unexpected(make_error("unexpected '}'", peek()));
        }
        auto directive_result = parse_directive();
        if (!directive_result) {
            return std::unexpected(directive_result.error());
        }
        document.directives.push_back(std::move(*directive_result));
    }
    return document;
}

const Token &Parser::peek() const noexcept {
    return tokens_[pos_];
}

const Token &Parser::next() noexcept {
    return tokens_[pos_++];
}

bool Parser::match(TokenKind kind) noexcept {
    if (peek().kind != kind) {
        return false;
    }
    ++pos_;
    return true;
}

std::expected<DirectiveNode, ConfigError> Parser::parse_directive() {
    if (peek().kind != TokenKind::Word) {
        return std::unexpected(make_error("expected directive name", peek()));
    }

    DirectiveNode directive;
    directive.location = peek().location;
    directive.name = next().text;

    while (peek().kind != TokenKind::Semicolon && peek().kind != TokenKind::LBrace) {
        if (peek().kind == TokenKind::End) {
            return std::unexpected(make_error("unexpected end of file while parsing directive", peek()));
        }
        if (peek().kind == TokenKind::RBrace) {
            return std::unexpected(make_error("unexpected '}' while parsing directive", peek()));
        }
        auto arg_result = parse_argument();
        if (!arg_result) {
            return std::unexpected(arg_result.error());
        }
        directive.args.push_back(std::move(*arg_result));
    }

    if (match(TokenKind::Semicolon)) {
        directive.has_block = false;
        return directive;
    }

    if (!match(TokenKind::LBrace)) {
        return std::unexpected(make_error("expected ';' or '{' after directive", peek()));
    }

    directive.has_block = true;
    while (peek().kind != TokenKind::RBrace) {
        if (peek().kind == TokenKind::End) {
            return std::unexpected(make_error("unexpected end of file inside block", peek()));
        }
        auto child_result = parse_directive();
        if (!child_result) {
            return std::unexpected(child_result.error());
        }
        directive.children.push_back(std::move(*child_result));
    }
    (void)match(TokenKind::RBrace);
    return directive;
}

std::expected<std::string, ConfigError> Parser::parse_argument() {
    if (peek().kind == TokenKind::Word || peek().kind == TokenKind::String) {
        return next().text;
    }
    if (peek().kind == TokenKind::Equal) {
        (void)next();
        return std::string("=");
    }
    return std::unexpected(make_error("expected directive argument", peek()));
}

ConfigError Parser::make_error(const char *message, const Token &token) const {
    return ConfigError{
        .message = message,
        .location = token.location,
    };
}

} // namespace fiber::lite_nginx::config
