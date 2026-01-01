#ifndef FIBER_SCRIPT_PARSE_TOKENIZER_H
#define FIBER_SCRIPT_PARSE_TOKENIZER_H

#include <expected>
#include <string>
#include <vector>

#include "Token.h"
#include "ParseError.h"

namespace fiber::script::parse {

class Tokenizer {
public:
    explicit Tokenizer(std::string input);

    std::expected<void, ParseError> process();

    const std::vector<Token> &tokens() const {
        return tokens_;
    }

private:
    std::string input_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::size_t max_ = 0;

    bool skip_comment();
    std::expected<void, ParseError> scan_string();
    std::expected<void, ParseError> lex_numeric_literal(bool first_char_zero);
    void lex_identifier();
    bool is_identifier(char ch) const;
    bool is_digit(char ch) const;
    bool is_alpha(char ch) const;
    bool is_hex_digit(char ch) const;
    bool is_char(char a, char b) const;
    bool is_exponent_char(char ch) const;
    bool is_float_suffix(char ch) const;
    bool is_double_suffix(char ch) const;
    bool is_sign(char ch) const;
    std::size_t js_line_terminator_length(std::size_t pos) const;
    std::expected<std::size_t, ParseError> scan_escape(char quote);

    void push_char_token(TokenKind kind);
    void push_pair_token(TokenKind kind);
    void push_three_token(TokenKind kind);
    void push_token(TokenKind kind, std::size_t start, std::size_t end, std::string text);
};

} // namespace fiber::script::parse

#endif // FIBER_SCRIPT_PARSE_TOKENIZER_H
