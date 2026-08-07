#include <fiber/script/parse/Tokenizer.h>

#include <cctype>

namespace fiber::script::parse {

Tokenizer::Tokenizer(std::string input, std::size_t max_depth) :
    input_(std::move(input)), max_depth_(max_depth == 0 ? 1 : max_depth) {
    max_ = input_.size();
}

std::expected<void, ParseError> Tokenizer::process() {
    tokens_.clear();
    pos_ = 0;
    max_ = input_.size();

    while (pos_ < max_) {
        char ch = input_[pos_];
        if (is_alpha(ch) || ch == '_' || ch == '$') {
            lex_identifier();
            continue;
        }
        switch (ch) {
            case '+':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '+') {
                    push_pair_token(TokenKind::Inc);
                } else {
                    push_char_token(TokenKind::Plus);
                }
                break;
            case '-':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '-') {
                    push_pair_token(TokenKind::Dec);
                } else {
                    push_char_token(TokenKind::Minus);
                }
                break;
            case ':':
                push_char_token(TokenKind::Colon);
                break;
            case '.':
                if (pos_ + 2 < max_ && input_[pos_ + 1] == '.' && input_[pos_ + 2] == '.') {
                    push_three_token(TokenKind::Expand);
                } else if (pos_ + 1 < max_ && is_digit(input_[pos_ + 1])) {
                    auto result = lex_numeric_literal(false, true);
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                } else {
                    push_char_token(TokenKind::Dot);
                }
                break;
            case ',':
                push_char_token(TokenKind::Comma);
                break;
            case '*':
                push_char_token(TokenKind::Star);
                break;
            case '/': {
                auto comment_result = skip_comment();
                if (!comment_result) {
                    return std::unexpected(comment_result.error());
                }
                if (!comment_result.value()) {
                    push_char_token(TokenKind::Div);
                }
                break;
            }
            case '%':
                push_char_token(TokenKind::Mod);
                break;
            case ';':
                push_char_token(TokenKind::Semicolon);
                break;
            case '(':
                push_char_token(TokenKind::LParen);
                break;
            case ')':
                push_char_token(TokenKind::RParen);
                break;
            case '[':
                push_char_token(TokenKind::LSquare);
                break;
            case ']':
                push_char_token(TokenKind::RSquare);
                break;
            case '{':
                push_char_token(TokenKind::LCurly);
                break;
            case '}':
                push_char_token(TokenKind::RCurly);
                break;
            case '#':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '[') {
                    push_pair_token(TokenKind::Project);
                } else {
                    push_char_token(TokenKind::Hash);
                }
                break;
            case '^':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '[') {
                    push_pair_token(TokenKind::Select);
                } else {
                    return std::unexpected(ParseError{"missing character after '^'", pos_});
                }
                break;
            case '~':
                push_char_token(TokenKind::Tilde);
                break;
            case '!':
                if (pos_ + 2 < max_ && input_[pos_ + 1] == '=' && input_[pos_ + 2] == '=') {
                    push_three_token(TokenKind::Sne);
                } else if (pos_ + 1 < max_ && input_[pos_ + 1] == '=') {
                    push_pair_token(TokenKind::Ne);
                } else {
                    push_char_token(TokenKind::Not);
                }
                break;
            case '=':
                if (pos_ + 2 < max_ && input_[pos_ + 1] == '=' && input_[pos_ + 2] == '=') {
                    push_three_token(TokenKind::Seq);
                } else if (pos_ + 1 < max_ && input_[pos_ + 1] == '=') {
                    push_pair_token(TokenKind::Eq);
                } else {
                    push_char_token(TokenKind::Assign);
                }
                break;
            case '&':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '&') {
                    push_pair_token(TokenKind::SymbolicAnd);
                } else {
                    return std::unexpected(ParseError{"missing character after '&'", pos_});
                }
                break;
            case '|':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '|') {
                    push_pair_token(TokenKind::SymbolicOr);
                } else {
                    return std::unexpected(ParseError{"missing character after '|'", pos_});
                }
                break;
            case '?':
                push_char_token(TokenKind::QMark);
                break;
            case '>':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '=') {
                    push_pair_token(TokenKind::GE);
                } else {
                    push_char_token(TokenKind::GT);
                }
                break;
            case '<':
                if (pos_ + 1 < max_ && input_[pos_ + 1] == '=') {
                    push_pair_token(TokenKind::LE);
                } else {
                    push_char_token(TokenKind::LT);
                }
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': {
                auto result = lex_numeric_literal(ch == '0');
                if (!result) {
                    return std::unexpected(result.error());
                }
                break;
            }
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                ++pos_;
                break;
            case '\'':
            case '\"': {
                auto result = scan_string();
                if (!result) {
                    return std::unexpected(result.error());
                }
                break;
            }
            case '`': {
                auto result = scan_template_literal();
                if (!result) {
                    return std::unexpected(result.error());
                }
                break;
            }
            case '\\':
                return std::unexpected(ParseError{"unexpected escape char", pos_});
            default:
                return std::unexpected(ParseError{"unexpected character", pos_});
        }
    }
    return {};
}

std::size_t Tokenizer::js_line_terminator_length(std::size_t pos) const {
    if (pos >= max_) {
        return 0;
    }
    unsigned char ch = static_cast<unsigned char>(input_[pos]);
    if (ch == '\n') {
        return 1;
    }
    if (ch == '\r') {
        if (pos + 1 < max_ && input_[pos + 1] == '\n') {
            return 2;
        }
        return 1;
    }
    if (ch == 0xE2 && pos + 2 < max_) {
        unsigned char next = static_cast<unsigned char>(input_[pos + 1]);
        unsigned char tail = static_cast<unsigned char>(input_[pos + 2]);
        if (next == 0x80 && (tail == 0xA8 || tail == 0xA9)) {
            return 3;
        }
    }
    return 0;
}

std::expected<bool, ParseError> Tokenizer::skip_comment() {
    std::size_t p = pos_ + 1;
    if (p < max_) {
        char c = input_[p++];
        if (c == '/') {
            while (p < max_ && js_line_terminator_length(p) == 0) {
                ++p;
            }
            std::size_t eol_len = js_line_terminator_length(p);
            pos_ = p + (eol_len != 0 ? eol_len : 1);
            return true;
        }
        if (c == '*') {
            bool closed = false;
            while (p + 1 < max_) {
                if (input_[p] == '*' && input_[p + 1] == '/') {
                    closed = true;
                    break;
                }
                ++p;
            }
            if (!closed) {
                return std::unexpected(ParseError{"unterminated comment", pos_});
            }
            pos_ = p + 2;
            return true;
        }
    }
    return false;
}

std::expected<void, ParseError> Tokenizer::scan_string() {
    char quote = input_[pos_];
    std::size_t start = pos_;
    ++pos_;
    while (pos_ < max_) {
        char chr = input_[pos_];
        if (chr == quote) {
            ++pos_;
            push_token(TokenKind::LiteralString, start, pos_, input_.substr(start, pos_ - start));
            return {};
        }
        if (js_line_terminator_length(pos_) != 0) {
            return std::unexpected(ParseError{"unterminated string literal", pos_});
        }
        if (chr == '\\') {
            ++pos_;
            auto result = scan_escape(quote);
            if (!result) {
                return std::unexpected(result.error());
            }
            pos_ += result.value();
            continue;
        }
        ++pos_;
    }
    return std::unexpected(ParseError{"unterminated string literal", start});
}

std::expected<void, ParseError> Tokenizer::scan_template_literal() {
    std::size_t start = pos_;
    auto end_result = skip_template_literal(pos_, 0);
    if (!end_result) {
        return std::unexpected(end_result.error());
    }
    pos_ = end_result.value();
    push_token(TokenKind::TemplateLiteral, start, pos_, input_.substr(start, pos_ - start));
    return {};
}

std::expected<std::size_t, ParseError> Tokenizer::skip_template_literal(std::size_t pos, std::size_t depth) const {
    if (depth >= max_depth_) {
        return std::unexpected(ParseError{"maximum template literal nesting depth exceeded", pos});
    }
    std::size_t p = pos + 1;
    while (p < max_) {
        char chr = input_[p];
        if (chr == '`') {
            return p + 1;
        }
        if (chr == '\\') {
            ++p;
            if (p >= max_) {
                return std::unexpected(ParseError{"unexpected escape", p});
            }
            std::size_t eol_len = js_line_terminator_length(p);
            p += eol_len != 0 ? eol_len : 1;
            continue;
        }
        if (chr == '$' && p + 1 < max_ && input_[p + 1] == '{') {
            auto expr_end = skip_template_expression(p + 2, depth);
            if (!expr_end) {
                return std::unexpected(expr_end.error());
            }
            p = expr_end.value();
            continue;
        }
        ++p;
    }
    return std::unexpected(ParseError{"unterminated template literal", pos});
}

std::expected<std::size_t, ParseError> Tokenizer::skip_template_expression(std::size_t pos, std::size_t depth) const {
    std::size_t p = pos;
    std::size_t curly_depth = 1;
    while (p < max_) {
        char chr = input_[p];
        if (chr == '\'' || chr == '"') {
            char quote = chr;
            ++p;
            while (p < max_) {
                if (input_[p] == quote) {
                    ++p;
                    break;
                }
                if (js_line_terminator_length(p) != 0) {
                    return std::unexpected(ParseError{"unterminated string literal", p});
                }
                if (input_[p] == '\\') {
                    ++p;
                    if (p >= max_) {
                        return std::unexpected(ParseError{"unexpected escape", p});
                    }
                    std::size_t eol_len = js_line_terminator_length(p);
                    p += eol_len != 0 ? eol_len : 1;
                    continue;
                }
                ++p;
            }
            if (p >= max_) {
                return std::unexpected(ParseError{"unterminated string literal", p});
            }
            continue;
        }
        if (chr == '`') {
            auto nested_end = skip_template_literal(p, depth + 1);
            if (!nested_end) {
                return std::unexpected(nested_end.error());
            }
            p = nested_end.value();
            continue;
        }
        if (chr == '/' && p + 1 < max_) {
            if (input_[p + 1] == '/') {
                p += 2;
                while (p < max_ && js_line_terminator_length(p) == 0) {
                    ++p;
                }
                continue;
            }
            if (input_[p + 1] == '*') {
                p += 2;
                while (p + 1 < max_ && !(input_[p] == '*' && input_[p + 1] == '/')) {
                    ++p;
                }
                if (p + 1 >= max_) {
                    return std::unexpected(ParseError{"unterminated comment", p});
                }
                p += 2;
                continue;
            }
        }
        if (chr == '{') {
            if (curly_depth >= max_depth_) {
                return std::unexpected(ParseError{"maximum template expression nesting depth exceeded", p});
            }
            ++curly_depth;
            ++p;
            continue;
        }
        if (chr == '}') {
            --curly_depth;
            ++p;
            if (curly_depth == 0) {
                return p;
            }
            continue;
        }
        ++p;
    }
    return std::unexpected(ParseError{"unterminated template expression", pos});
}

std::expected<std::size_t, ParseError> Tokenizer::scan_escape(char quote) {
    if (pos_ >= max_) {
        return std::unexpected(ParseError{"unexpected escape", pos_});
    }
    std::size_t eol_len = js_line_terminator_length(pos_);
    if (eol_len != 0) {
        return eol_len;
    }
    char chr = input_[pos_];
    std::size_t p = pos_;
    int length = 0;
    int base = 0;
    switch (chr) {
        case 'a':
        case '\'':
        case '\"':
        case '`':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
        case 'v':
        case '\\':
            return 1;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            // octal escape: 1 to 3 octal digits, the first being this char (matches
            // parse_string_literal, which also accepts 1-3 digits).
            p = pos_;
            int count = 1;
            while (count < 3 && p + 1 < max_) {
                char c = input_[p + 1];
                if (c < '0' || c > '7') {
                    break;
                }
                ++p;
                ++count;
            }
            return p - pos_ + 1;
        }
        case 'x':
            p = pos_ + 1;
            length = 2;
            base = 16;
            break;
        case 'u':
            p = pos_ + 1;
            length = 4;
            base = 16;
            break;
        default:
            return std::unexpected(ParseError{"unexpected escape", pos_});
    }

    int l = length;
    for (; l > 0 && p < max_; --l, ++p) {
        char c = input_[p];
        int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return std::unexpected(ParseError{"unexpected escape", p});
        }
        if (digit >= base) {
            return std::unexpected(ParseError{"unexpected escape", p});
        }
    }
    if (l != 0) {
        return std::unexpected(ParseError{"unexpected escape", p});
    }
    return p - pos_;
}

std::expected<void, ParseError> Tokenizer::lex_numeric_literal(bool first_char_zero, bool leading_dot) {
    bool is_real = false;
    std::size_t start = pos_;
    char ch = pos_ + 1 < max_ ? input_[pos_ + 1] : '\0';
    bool is_hex = ch == 'x' || ch == 'X';

    if (first_char_zero && is_hex) {
        pos_ += 1;
        do {
            ++pos_;
        } while (pos_ < max_ && is_hex_digit(input_[pos_]));
        std::size_t end = pos_;
        if (end <= start + 2) {
            return std::unexpected(ParseError{"invalid hex literal", start});
        }
        std::string digits = input_.substr(start + 2, end - (start + 2));
        bool is_long = is_char('L', 'l');
        push_token(is_long ? TokenKind::LiteralHexLong : TokenKind::LiteralHexInt, start, end, digits);
        if (is_long) {
            ++pos_;
        }
        return {};
    }

    if (leading_dot) {
        is_real = true;
        ++pos_;
        while (pos_ < max_ && is_digit(input_[pos_])) {
            ++pos_;
        }
    } else {
        do {
            ++pos_;
        } while (pos_ < max_ && is_digit(input_[pos_]));
        if (pos_ < max_ && input_[pos_] == '.') {
            is_real = true;
            ++pos_;
            while (pos_ < max_ && is_digit(input_[pos_])) {
                ++pos_;
            }
        }
    }

    std::size_t end_of_number = pos_;
    if (pos_ >= max_) {
        push_token(is_real ? TokenKind::LiteralReal : TokenKind::LiteralInt, start, end_of_number,
                   input_.substr(start, end_of_number - start));
        return {};
    }

    if (is_char('L', 'l')) {
        if (is_real) {
            return std::unexpected(ParseError{"real cannot be long", start});
        }
        push_token(TokenKind::LiteralLong, start, end_of_number, input_.substr(start, end_of_number - start));
        ++pos_;
        return {};
    }

    if (is_exponent_char(input_[pos_])) {
        is_real = true;
        ++pos_;
        if (pos_ < max_ && is_sign(input_[pos_])) {
            ++pos_;
        }
        while (pos_ < max_ && is_digit(input_[pos_])) {
            ++pos_;
        }
        bool is_float = false;
        if (pos_ < max_ && is_float_suffix(input_[pos_])) {
            is_float = true;
            ++pos_;
        } else if (pos_ < max_ && is_double_suffix(input_[pos_])) {
            ++pos_;
        }
        push_token(is_float ? TokenKind::LiteralRealFloat : TokenKind::LiteralReal, start, pos_,
                   input_.substr(start, pos_ - start));
        return {};
    }

    bool is_float = false;
    if (pos_ < max_ && is_float_suffix(input_[pos_])) {
        is_real = true;
        is_float = true;
        ++pos_;
    } else if (pos_ < max_ && is_double_suffix(input_[pos_])) {
        is_real = true;
        ++pos_;
    }

    if (is_real) {
        push_token(is_float ? TokenKind::LiteralRealFloat : TokenKind::LiteralReal, start, pos_,
                   input_.substr(start, pos_ - start));
    } else {
        push_token(TokenKind::LiteralInt, start, end_of_number, input_.substr(start, end_of_number - start));
    }
    return {};
}

void Tokenizer::lex_identifier() {
    std::size_t start = pos_;
    do {
        ++pos_;
    } while (pos_ < max_ && is_identifier(input_[pos_]));
    push_token(TokenKind::Identifier, start, pos_, input_.substr(start, pos_ - start));
}

bool Tokenizer::is_identifier(char ch) const { return is_alpha(ch) || is_digit(ch) || ch == '_' || ch == '$'; }

bool Tokenizer::is_digit(char ch) const {
    if (static_cast<unsigned char>(ch) > 255) {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(ch)) != 0;
}

bool Tokenizer::is_alpha(char ch) const {
    if (static_cast<unsigned char>(ch) > 255) {
        return false;
    }
    return std::isalpha(static_cast<unsigned char>(ch)) != 0;
}

bool Tokenizer::is_hex_digit(char ch) const {
    if (static_cast<unsigned char>(ch) > 255) {
        return false;
    }
    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

bool Tokenizer::is_char(char a, char b) const {
    if (pos_ >= max_) {
        return false;
    }
    char ch = input_[pos_];
    return ch == a || ch == b;
}

bool Tokenizer::is_exponent_char(char ch) const { return ch == 'e' || ch == 'E'; }

bool Tokenizer::is_float_suffix(char ch) const { return ch == 'f' || ch == 'F'; }

bool Tokenizer::is_double_suffix(char ch) const { return ch == 'd' || ch == 'D'; }

bool Tokenizer::is_sign(char ch) const { return ch == '+' || ch == '-'; }

void Tokenizer::push_char_token(TokenKind kind) {
    tokens_.push_back({kind, pos_, pos_ + 1, std::string(1, input_[pos_])});
    ++pos_;
}

void Tokenizer::push_pair_token(TokenKind kind) {
    tokens_.push_back({kind, pos_, pos_ + 2, input_.substr(pos_, 2)});
    pos_ += 2;
}

void Tokenizer::push_three_token(TokenKind kind) {
    tokens_.push_back({kind, pos_, pos_ + 3, input_.substr(pos_, 3)});
    pos_ += 3;
}

void Tokenizer::push_token(TokenKind kind, std::size_t start, std::size_t end, std::string text) {
    tokens_.push_back({kind, start, end, std::move(text)});
}

} // namespace fiber::script::parse
