#include "Parser.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <unordered_set>

namespace fiber::script::parse {

namespace {

bool is_keyword(std::string_view text) {
    return text == "let" || text == "if" || text == "else" || text == "for" || text == "of" ||
           text == "continue" || text == "break" || text == "return" || text == "directive" ||
           text == "try" || text == "catch" || text == "throw";
}

void append_utf8(std::string &out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

} // namespace

Parser::Parser(Library &library, bool allow_assign)
    : library_(library), allow_assign_(allow_assign) {
}

std::expected<std::unique_ptr<ast::Block>, ParseError> Parser::parse_script(std::string_view script) {
    Tokenizer tokenizer{std::string(script)};
    auto token_result = tokenizer.process();
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    tokens_ = tokenizer.tokens();
    pos_ = 0;
    directive_map_.clear();
    directive_statements_.clear();

    if (!has_more()) {
        return std::unexpected(make_error("unexpected end of input", nullptr));
    }
    auto block_result = parse_block(false, ast::BlockType::Script);
    if (!block_result) {
        return std::unexpected(block_result.error());
    }
    if (has_more()) {
        return std::unexpected(make_error("unexpected token after script", peek()));
    }
    return block_result;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_expression(std::string_view expression) {
    Tokenizer tokenizer{std::string(expression)};
    auto token_result = tokenizer.process();
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    tokens_ = tokenizer.tokens();
    pos_ = 0;
    directive_map_.clear();
    directive_statements_.clear();

    if (!has_more()) {
        return std::unexpected(make_error("empty expression", nullptr));
    }
    auto expr_result = parse_expression_internal();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    if (peek(TokenKind::Semicolon, true)) {
        while (peek(TokenKind::Semicolon, true)) {
        }
    }
    if (has_more()) {
        return std::unexpected(make_error("unexpected token after expression", peek()));
    }
    return expr_result;
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_statement() {
    if (!has_more()) {
        return std::unexpected(make_error("unexpected end of input", nullptr));
    }

    if (peek_identifier("if")) {
        return parse_if_statement();
    }
    if (peek_identifier("for")) {
        return parse_foreach_statement();
    }
    if (peek_identifier("break")) {
        return parse_break_statement();
    }
    if (peek_identifier("continue")) {
        return parse_continue_statement();
    }
    if (peek_identifier("return")) {
        return parse_return_statement();
    }
    if (peek_identifier("throw")) {
        return parse_throw_statement();
    }
    if (peek_identifier("try")) {
        return parse_try_catch_statement();
    }
    if (peek_identifier("let")) {
        return parse_variable_declare_statement();
    }
    if (peek_identifier("directive")) {
        return parse_directive_statement();
    }
    if (peek(TokenKind::LCurly)) {
        auto block_result = parse_block(true, ast::BlockType::Script);
        if (!block_result) {
            return std::unexpected(block_result.error());
        }
        return std::unique_ptr<ast::Statement>(std::move(block_result.value()));
    }

    auto expr_result = parse_expression_internal();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto expr = std::move(expr_result.value());
    std::int32_t start = expr->start_pos();
    std::int32_t end = expr->end_pos();
    if (peek(TokenKind::Semicolon, true)) {
        end = static_cast<std::int32_t>(tokens_[pos_ - 1].end);
    }
    return std::make_unique<ast::ExpressionStatement>(start, end, std::move(expr));
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_break_statement() {
    auto token_result = eat_keyword("break");
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    std::int32_t start = static_cast<std::int32_t>(token_result->start);
    std::int32_t end = static_cast<std::int32_t>(token_result->end);
    if (peek(TokenKind::Semicolon, true)) {
        end = static_cast<std::int32_t>(tokens_[pos_ - 1].end);
    }
    return std::make_unique<ast::BreakStatement>(start, end);
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_continue_statement() {
    auto token_result = eat_keyword("continue");
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    std::int32_t start = static_cast<std::int32_t>(token_result->start);
    std::int32_t end = static_cast<std::int32_t>(token_result->end);
    if (peek(TokenKind::Semicolon, true)) {
        end = static_cast<std::int32_t>(tokens_[pos_ - 1].end);
    }
    return std::make_unique<ast::ContinueStatement>(start, end);
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_return_statement() {
    auto token_result = eat_keyword("return");
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    std::unique_ptr<ast::Expression> value;
    if (has_more() && !peek(TokenKind::Semicolon) && !peek(TokenKind::RCurly)) {
        auto expr_result = parse_expression_internal();
        if (!expr_result) {
            return std::unexpected(expr_result.error());
        }
        value = std::move(expr_result.value());
    }
    std::int32_t start = static_cast<std::int32_t>(token_result->start);
    std::int32_t end = static_cast<std::int32_t>(token_result->end);
    if (value) {
        end = value->end_pos();
    }
    if (peek(TokenKind::Semicolon, true)) {
        end = static_cast<std::int32_t>(tokens_[pos_ - 1].end);
    }
    return std::make_unique<ast::ReturnStatement>(start, end, std::move(value));
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_throw_statement() {
    auto token_result = eat_keyword("throw");
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    auto expr_result = parse_expression_internal();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    std::int32_t start = static_cast<std::int32_t>(token_result->start);
    std::int32_t end = expr_result.value()->end_pos();
    if (peek(TokenKind::Semicolon, true)) {
        end = static_cast<std::int32_t>(tokens_[pos_ - 1].end);
    }
    return std::make_unique<ast::ThrowStatement>(start, end, std::move(expr_result.value()));
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_try_catch_statement() {
    auto try_token = eat_keyword("try");
    if (!try_token) {
        return std::unexpected(try_token.error());
    }
    auto try_block_result = parse_block(true, ast::BlockType::TryBlock);
    if (!try_block_result) {
        return std::unexpected(try_block_result.error());
    }
    auto catch_token = eat_keyword("catch");
    if (!catch_token) {
        return std::unexpected(catch_token.error());
    }
    auto lp_result = eat(TokenKind::LParen);
    if (!lp_result) {
        return std::unexpected(lp_result.error());
    }
    auto identifier_result = parse_identifier_token();
    if (!identifier_result) {
        return std::unexpected(identifier_result.error());
    }
    auto rp_result = eat(TokenKind::RParen);
    if (!rp_result) {
        return std::unexpected(rp_result.error());
    }
    auto catch_block_result = parse_block(true, ast::BlockType::CatchBlock);
    if (!catch_block_result) {
        return std::unexpected(catch_block_result.error());
    }
    std::int32_t start = static_cast<std::int32_t>(try_token->start);
    std::int32_t end = catch_block_result.value()->end_pos();
    return std::make_unique<ast::TryCatchStatement>(start,
                                                    end,
                                                    std::make_unique<ast::Identifier>(std::move(identifier_result.value())),
                                                    std::move(try_block_result.value()),
                                                    std::move(catch_block_result.value()));
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_if_statement() {
    auto if_token = eat_keyword("if");
    if (!if_token) {
        return std::unexpected(if_token.error());
    }
    auto lp_result = eat(TokenKind::LParen);
    if (!lp_result) {
        return std::unexpected(lp_result.error());
    }
    auto cond_result = parse_expression_internal();
    if (!cond_result) {
        return std::unexpected(cond_result.error());
    }
    auto rp_result = eat(TokenKind::RParen);
    if (!rp_result) {
        return std::unexpected(rp_result.error());
    }
    auto true_block_result = parse_block(true, ast::BlockType::IfBlock);
    if (!true_block_result) {
        return std::unexpected(true_block_result.error());
    }
    std::unique_ptr<ast::Statement> else_stmt;
    if (peek_identifier("else")) {
        next();
        if (peek_identifier("if")) {
            auto else_if = parse_if_statement();
            if (!else_if) {
                return std::unexpected(else_if.error());
            }
            else_stmt = std::move(else_if.value());
        } else {
            auto else_block_result = parse_block(true, ast::BlockType::ElseBlock);
            if (!else_block_result) {
                return std::unexpected(else_block_result.error());
            }
            else_stmt = std::move(else_block_result.value());
        }
    }
    std::int32_t start = static_cast<std::int32_t>(if_token->start);
    std::int32_t end = true_block_result.value()->end_pos();
    if (else_stmt) {
        end = else_stmt->end_pos();
    }
    return std::make_unique<ast::IfStatement>(start,
                                              end,
                                              std::move(cond_result.value()),
                                              std::move(true_block_result.value()),
                                              std::move(else_stmt));
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_foreach_statement() {
    auto for_token = eat_keyword("for");
    if (!for_token) {
        return std::unexpected(for_token.error());
    }
    auto lp_result = eat(TokenKind::LParen);
    if (!lp_result) {
        return std::unexpected(lp_result.error());
    }
    auto let_result = eat_keyword("let");
    if (!let_result) {
        return std::unexpected(let_result.error());
    }
    auto key_result = parse_identifier_token();
    if (!key_result) {
        return std::unexpected(key_result.error());
    }
    auto comma_result = eat(TokenKind::Comma);
    if (!comma_result) {
        return std::unexpected(comma_result.error());
    }
    auto value_result = parse_identifier_token();
    if (!value_result) {
        return std::unexpected(value_result.error());
    }
    auto of_result = eat_keyword("of");
    if (!of_result) {
        return std::unexpected(of_result.error());
    }
    auto collection_result = parse_expression_internal();
    if (!collection_result) {
        return std::unexpected(collection_result.error());
    }
    auto rp_result = eat(TokenKind::RParen);
    if (!rp_result) {
        return std::unexpected(rp_result.error());
    }
    auto block_result = parse_block(true, ast::BlockType::ForBlock);
    if (!block_result) {
        return std::unexpected(block_result.error());
    }
    std::int32_t start = static_cast<std::int32_t>(for_token->start);
    std::int32_t end = block_result.value()->end_pos();
    return std::make_unique<ast::ForeachStatement>(start,
                                                   end,
                                                   std::make_unique<ast::Identifier>(std::move(key_result.value())),
                                                   std::make_unique<ast::Identifier>(std::move(value_result.value())),
                                                   std::move(collection_result.value()),
                                                   std::move(block_result.value()));
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_variable_declare_statement() {
    auto let_token = eat_keyword("let");
    if (!let_token) {
        return std::unexpected(let_token.error());
    }
    auto identifier_result = parse_identifier_token();
    if (!identifier_result) {
        return std::unexpected(identifier_result.error());
    }
    std::unique_ptr<ast::Expression> initializer;
    if (peek(TokenKind::Assign, true)) {
        auto expr_result = parse_expression_internal();
        if (!expr_result) {
            return std::unexpected(expr_result.error());
        }
        initializer = std::move(expr_result.value());
    }
    auto semi_result = eat(TokenKind::Semicolon);
    if (!semi_result) {
        return std::unexpected(semi_result.error());
    }
    std::int32_t start = static_cast<std::int32_t>(let_token->start);
    std::int32_t end = static_cast<std::int32_t>(semi_result->end);
    return std::make_unique<ast::VariableDeclareStatement>(start,
                                                           end,
                                                           std::make_unique<ast::Identifier>(std::move(identifier_result.value())),
                                                           std::move(initializer));
}

std::expected<std::unique_ptr<ast::Statement>, ParseError> Parser::parse_directive_statement() {
    auto directive_token = eat_keyword("directive");
    if (!directive_token) {
        return std::unexpected(directive_token.error());
    }
    auto name_result = parse_identifier_token();
    if (!name_result) {
        return std::unexpected(name_result.error());
    }
    if (!peek(TokenKind::Assign, true)) {
        const Token *token = peek();
        if (!token || token->kind != TokenKind::Identifier || token->text != "from") {
            return std::unexpected(make_error("directive missing '='", peek()));
        }
        next();
    }
    auto type_result = parse_identifier_token();
    if (!type_result) {
        return std::unexpected(type_result.error());
    }
    std::vector<ast::Literal> literals;
    while (true) {
        auto literal_result = parse_optional_literal();
        if (!literal_result) {
            return std::unexpected(literal_result.error());
        }
        if (!literal_result.value()) {
            break;
        }
        literals.push_back(std::move(*literal_result.value()));
    }

    auto semi_result = eat(TokenKind::Semicolon);
    if (!semi_result) {
        return std::unexpected(semi_result.error());
    }

    std::string name = name_result->name();
    if (directive_map_.find(name) != directive_map_.end()) {
        return std::unexpected(make_error("directive exists", directive_token ? &directive_token.value() : nullptr));
    }

    std::vector<fiber::json::JsValue> literal_values;
    for (const auto &lit : literals) {
        switch (lit.kind()) {
            case ast::Literal::Kind::NullValue:
                literal_values.push_back(fiber::json::JsValue::make_null());
                break;
            case ast::Literal::Kind::Boolean:
                literal_values.push_back(fiber::json::JsValue::make_boolean(lit.bool_value()));
                break;
            case ast::Literal::Kind::Integer:
                literal_values.push_back(fiber::json::JsValue::make_integer(lit.int_value()));
                break;
            case ast::Literal::Kind::Float:
                literal_values.push_back(fiber::json::JsValue::make_float(lit.float_value()));
                break;
            case ast::Literal::Kind::String:
                literal_values.push_back(fiber::json::JsValue::make_native_string(
                    const_cast<char *>(lit.string_value().data()),
                    lit.string_value().size()));
                break;
        }
    }

    Library::DirectiveDef *def = library_.find_directive_def(type_result->name(), name_result->name(), literal_values);
    if (!def) {
        return std::unexpected(make_error("directive not found", directive_token ? &directive_token.value() : nullptr));
    }
    std::int32_t start = static_cast<std::int32_t>(directive_token->start);
    std::int32_t end = static_cast<std::int32_t>(semi_result->end);
    auto stmt = std::make_unique<ast::DirectiveStatement>(start,
                                                          end,
                                                          std::make_unique<ast::Identifier>(std::move(type_result.value())),
                                                          std::make_unique<ast::Identifier>(std::move(name_result.value())),
                                                          def);
    return stmt;
}

std::expected<std::unique_ptr<ast::Block>, ParseError> Parser::parse_block(bool must_curly, ast::BlockType type) {
    std::size_t start_pos = 0;
    if (must_curly) {
        auto token_result = eat(TokenKind::LCurly);
        if (!token_result) {
            return std::unexpected(token_result.error());
        }
        start_pos = token_result->start;
    } else if (has_more()) {
        start_pos = peek()->start;
    }

    auto block = std::make_unique<ast::Block>(static_cast<std::int32_t>(start_pos),
                                              static_cast<std::int32_t>(start_pos),
                                              type);
    bool has_statement = false;
    while (has_more()) {
        if (must_curly && peek(TokenKind::RCurly)) {
            auto end_token = eat(TokenKind::RCurly);
            if (!end_token) {
                return std::unexpected(end_token.error());
            }
            block->set_range(static_cast<std::int32_t>(start_pos), static_cast<std::int32_t>(end_token->end));
            return block;
        }
        if (peek(TokenKind::Semicolon, true)) {
            continue;
        }
        auto stmt_result = parse_statement();
        if (!stmt_result) {
            return std::unexpected(stmt_result.error());
        }
        auto stmt = std::move(stmt_result.value());
        if (auto *directive = dynamic_cast<ast::DirectiveStatement *>(stmt.get())) {
            auto directive_ptr =
                std::unique_ptr<ast::DirectiveStatement>(static_cast<ast::DirectiveStatement *>(stmt.release()));
            directive_map_[directive_ptr->name()->name()] = directive_ptr.get();
            directive_statements_.push_back(std::move(directive_ptr));
            continue;
        }
        if (!has_statement) {
            block->set_range(stmt->start_pos(), stmt->end_pos());
            has_statement = true;
        } else {
            block->set_range(block->start_pos(), stmt->end_pos());
        }
        block->add_statement(std::move(stmt));
    }
    if (must_curly) {
        return std::unexpected(make_error("expected '}'", nullptr));
    }
    if (!has_statement) {
        return std::unexpected(make_error("unexpected end of input", nullptr));
    }
    return block;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_expression_internal() {
    auto expr_result = parse_logical_or();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto expr = std::move(expr_result.value());

    if (has_more()) {
        if (allow_assign_ && peek(TokenKind::Assign)) {
            next();
            auto rhs_result = parse_logical_or();
            if (!rhs_result) {
                return std::unexpected(rhs_result.error());
            }
            auto *lvalue = dynamic_cast<ast::MaybeLValue *>(expr.get());
            if (!lvalue) {
                return std::unexpected(make_error("assignment requires lvalue", peek()));
            }
            lvalue->mark_lvalue();
            auto left_ptr = std::unique_ptr<ast::MaybeLValue>(static_cast<ast::MaybeLValue *>(expr.release()));
            std::int32_t start = left_ptr->start_pos();
            std::int32_t end = rhs_result.value()->end_pos();
            return std::make_unique<ast::Assign>(start, end, std::move(left_ptr), std::move(rhs_result.value()));
        }
        if (peek(TokenKind::QMark)) {
            next();
            auto true_result = parse_expression_internal();
            if (!true_result) {
                return std::unexpected(true_result.error());
            }
            auto colon_result = eat(TokenKind::Colon);
            if (!colon_result) {
                return std::unexpected(colon_result.error());
            }
            auto false_result = parse_expression_internal();
            if (!false_result) {
                return std::unexpected(false_result.error());
            }
            std::int32_t start = expr->start_pos();
            std::int32_t end = false_result.value()->end_pos();
            return std::make_unique<ast::Ternary>(start,
                                                  end,
                                                  std::move(expr),
                                                  std::move(true_result.value()),
                                                  std::move(false_result.value()));
        }
    }
    return expr;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_logical_or() {
    auto expr_result = parse_logical_and();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto expr = std::move(expr_result.value());
    while (peek(TokenKind::SymbolicOr)) {
        auto token = next();
        auto rhs_result = parse_logical_and();
        if (!rhs_result) {
            return std::unexpected(rhs_result.error());
        }
        std::int32_t start = expr->start_pos();
        std::int32_t end = rhs_result.value()->end_pos();
        expr = std::make_unique<ast::LogicRelationalExpression>(start,
                                                                end,
                                                                std::move(expr),
                                                                ast::Operator::Or,
                                                                std::move(rhs_result.value()));
    }
    return expr;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_logical_and() {
    auto expr_result = parse_relational();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto expr = std::move(expr_result.value());
    while (peek(TokenKind::SymbolicAnd)) {
        auto token = next();
        auto rhs_result = parse_relational();
        if (!rhs_result) {
            return std::unexpected(rhs_result.error());
        }
        std::int32_t start = expr->start_pos();
        std::int32_t end = rhs_result.value()->end_pos();
        expr = std::make_unique<ast::LogicRelationalExpression>(start,
                                                                end,
                                                                std::move(expr),
                                                                ast::Operator::And,
                                                                std::move(rhs_result.value()));
    }
    return expr;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_relational() {
    auto expr_result = parse_sum();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto expr = std::move(expr_result.value());
    const Token *token = peek();
    if (!token) {
        return expr;
    }
    if (token->is_numeric_relational_operator() || token->kind == TokenKind::Tilde ||
        (token->kind == TokenKind::Identifier && token->text == "in")) {
        Token op_token = *next();
        auto rhs_result = parse_sum();
        if (!rhs_result) {
            return std::unexpected(rhs_result.error());
        }
        ast::Operator op = ast::Operator::Eq;
        if (op_token.kind == TokenKind::Tilde) {
            op = ast::Operator::Match;
        } else if (op_token.kind == TokenKind::Identifier && op_token.text == "in") {
            op = ast::Operator::In;
        } else {
            auto mapped = ast::operator_from_token(op_token.kind);
            if (!mapped) {
                return std::unexpected(make_error("unsupported operator", &op_token));
            }
            op = *mapped;
        }
        std::int32_t start = expr->start_pos();
        std::int32_t end = rhs_result.value()->end_pos();
        return std::make_unique<ast::BinaryOperator>(start, end, op, std::move(expr), std::move(rhs_result.value()));
    }
    return expr;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_sum() {
    auto expr_result = parse_product();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto expr = std::move(expr_result.value());
    while (peek(TokenKind::Plus, TokenKind::Minus)) {
        Token op_token = *next();
        auto rhs_result = parse_product();
        if (!rhs_result) {
            return std::unexpected(rhs_result.error());
        }
        auto mapped = ast::operator_from_token(op_token.kind);
        if (!mapped) {
            return std::unexpected(make_error("unsupported operator", &op_token));
        }
        std::int32_t start = expr->start_pos();
        std::int32_t end = rhs_result.value()->end_pos();
        expr = std::make_unique<ast::BinaryOperator>(start, end, *mapped, std::move(expr), std::move(rhs_result.value()));
    }
    return expr;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_product() {
    auto expr_result = parse_unary();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto expr = std::move(expr_result.value());
    while (peek(TokenKind::Star, TokenKind::Div, TokenKind::Mod)) {
        Token op_token = *next();
        auto rhs_result = parse_unary();
        if (!rhs_result) {
            return std::unexpected(rhs_result.error());
        }
        auto mapped = ast::operator_from_token(op_token.kind);
        if (!mapped) {
            return std::unexpected(make_error("unsupported operator", &op_token));
        }
        std::int32_t start = expr->start_pos();
        std::int32_t end = rhs_result.value()->end_pos();
        expr = std::make_unique<ast::BinaryOperator>(start, end, *mapped, std::move(expr), std::move(rhs_result.value()));
    }
    return expr;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_unary() {
    if (!has_more()) {
        return std::unexpected(make_error("unexpected end of input", nullptr));
    }
    const Token *token = peek();
    if (token->kind == TokenKind::Plus || token->kind == TokenKind::Minus || token->kind == TokenKind::Not) {
        Token op_token = *next();
        auto rhs_result = parse_unary();
        if (!rhs_result) {
            return std::unexpected(rhs_result.error());
        }
        auto mapped = ast::operator_from_token(op_token.kind);
        if (!mapped) {
            return std::unexpected(make_error("unsupported unary operator", &op_token));
        }
        std::int32_t start = static_cast<std::int32_t>(op_token.start);
        std::int32_t end = rhs_result.value()->end_pos();
        return std::make_unique<ast::UnaryOperator>(start, end, *mapped, std::move(rhs_result.value()));
    }
    if (token->kind == TokenKind::Identifier && token->text == "typeof") {
        Token op_token = *next();
        auto rhs_result = parse_unary();
        if (!rhs_result) {
            return std::unexpected(rhs_result.error());
        }
        std::int32_t start = static_cast<std::int32_t>(op_token.start);
        std::int32_t end = rhs_result.value()->end_pos();
        return std::make_unique<ast::UnaryOperator>(start, end, ast::Operator::Typeof, std::move(rhs_result.value()));
    }
    return parse_primary();
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_primary() {
    auto start_result = parse_start_node();
    if (!start_result) {
        return std::unexpected(start_result.error());
    }
    auto start = std::move(start_result.value());

    if (auto *var_ref = dynamic_cast<ast::VariableReference *>(start.get())) {
        if (!var_ref->is_root()) {
            auto func_result = parse_function_call(*var_ref);
            if (func_result) {
                start = std::move(func_result.value());
            } else if (!func_result.error().message.empty()) {
                return std::unexpected(func_result.error());
            }
        }
    }

    while (peek(TokenKind::Dot) || peek(TokenKind::LSquare)) {
        auto node_result = parse_node(std::move(start));
        if (!node_result) {
            return std::unexpected(node_result.error());
        }
        start = std::move(node_result.value());
    }
    return start;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_start_node() {
    auto literal_result = parse_literal();
    if (literal_result) {
        return literal_result;
    }
    auto list_result = parse_inline_list();
    if (list_result) {
        return list_result;
    }
    auto obj_result = parse_inline_object();
    if (obj_result) {
        return obj_result;
    }
    auto paren_result = parse_paren_expression();
    if (paren_result) {
        return paren_result;
    }
    return parse_function_or_var();
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_paren_expression() {
    if (!peek(TokenKind::LParen)) {
        return std::unexpected(ParseError{});
    }
    next();
    auto expr_result = parse_expression_internal();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto rp_result = eat(TokenKind::RParen);
    if (!rp_result) {
        return std::unexpected(rp_result.error());
    }
    return expr_result;
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_literal() {
    auto literal_result = parse_optional_literal();
    if (!literal_result) {
        return std::unexpected(literal_result.error());
    }
    if (!literal_result.value()) {
        return std::unexpected(ParseError{});
    }
    return std::make_unique<ast::Literal>(std::move(*literal_result.value()));
}

std::expected<std::optional<ast::Literal>, ParseError> Parser::parse_optional_literal() {
    const Token *token = peek();
    if (!token) {
        return std::optional<ast::Literal>{};
    }
    if (token->kind == TokenKind::Identifier) {
        if (token->text == "true" || token->text == "false") {
            bool value = token->text == "true";
            next();
            return std::optional<ast::Literal>(ast::Literal(static_cast<std::int32_t>(token->start),
                                                            static_cast<std::int32_t>(token->end),
                                                            value));
        }
        if (token->text == "null") {
            next();
            return std::optional<ast::Literal>(ast::Literal::make_null(static_cast<std::int32_t>(token->start),
                                                                       static_cast<std::int32_t>(token->end)));
        }
        return std::optional<ast::Literal>{};
    }

    if (token->kind == TokenKind::LiteralInt || token->kind == TokenKind::LiteralLong ||
        token->kind == TokenKind::LiteralHexInt || token->kind == TokenKind::LiteralHexLong ||
        token->kind == TokenKind::LiteralReal || token->kind == TokenKind::LiteralRealFloat ||
        token->kind == TokenKind::LiteralString) {
        auto literal_result = parse_literal_token(*token);
        if (!literal_result) {
            return std::unexpected(literal_result.error());
        }
        next();
        return std::optional<ast::Literal>(std::move(literal_result.value()));
    }
    return std::optional<ast::Literal>{};
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_inline_list() {
    if (!peek(TokenKind::LSquare)) {
        return std::unexpected(ParseError{});
    }
    Token start = *next();
    std::vector<std::unique_ptr<ast::Expression>> values;
    if (peek(TokenKind::RSquare, true)) {
        return std::make_unique<ast::InlineList>(static_cast<std::int32_t>(start.start),
                                                 static_cast<std::int32_t>(tokens_[pos_ - 1].end),
                                                 std::move(values));
    }
    while (has_more()) {
        if (peek(TokenKind::RSquare)) {
            break;
        }
        std::unique_ptr<ast::Expression> expr;
        if (peek(TokenKind::Expand)) {
            Token expand_token = *next();
            auto inner_result = parse_expression_internal();
            if (!inner_result) {
                return std::unexpected(inner_result.error());
            }
            std::int32_t end = inner_result.value()->end_pos();
            expr = std::make_unique<ast::ExpandArrArg>(static_cast<std::int32_t>(expand_token.start),
                                                       end,
                                                       std::move(inner_result.value()),
                                                       ast::ExpandArrArg::Where::InitArr);
        } else {
            auto expr_result = parse_expression_internal();
            if (!expr_result) {
                return std::unexpected(expr_result.error());
            }
            expr = std::move(expr_result.value());
        }
        values.push_back(std::move(expr));
        if (peek(TokenKind::Comma, true)) {
            continue;
        }
        break;
    }
    auto end_token = eat(TokenKind::RSquare);
    if (!end_token) {
        return std::unexpected(end_token.error());
    }
    return std::make_unique<ast::InlineList>(static_cast<std::int32_t>(start.start),
                                             static_cast<std::int32_t>(end_token->end),
                                             std::move(values));
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_inline_object() {
    if (!peek(TokenKind::LCurly)) {
        return std::unexpected(ParseError{});
    }
    Token start = *next();
    std::vector<ast::InlineObject::Entry> entries;
    std::unordered_set<std::string> string_keys;
    if (peek(TokenKind::RCurly, true)) {
        return std::make_unique<ast::InlineObject>(static_cast<std::int32_t>(start.start),
                                                   static_cast<std::int32_t>(tokens_[pos_ - 1].end),
                                                   std::move(entries));
    }
    while (has_more()) {
        const Token *token = peek();
        if (!token) {
            return std::unexpected(make_error("unexpected end in object", nullptr));
        }
        if (token->kind == TokenKind::RCurly) {
            break;
        }
        if (token->kind == TokenKind::Expand) {
            Token expand_token = *next();
            auto inner_result = parse_expression_internal();
            if (!inner_result) {
                return std::unexpected(inner_result.error());
            }
            ast::InlineObject::Key key;
            key.kind = ast::InlineObject::KeyKind::Expand;
            ast::InlineObject::Entry entry;
            entry.key = std::move(key);
            entry.value = std::make_unique<ast::ExpandArrArg>(static_cast<std::int32_t>(expand_token.start),
                                                              inner_result.value()->end_pos(),
                                                              std::move(inner_result.value()),
                                                              ast::ExpandArrArg::Where::InitObj);
            entries.push_back(std::move(entry));
        } else if (token->kind == TokenKind::LSquare) {
            next();
            auto key_expr_result = parse_expression_internal();
            if (!key_expr_result) {
                return std::unexpected(key_expr_result.error());
            }
            auto rsq = eat(TokenKind::RSquare);
            if (!rsq) {
                return std::unexpected(rsq.error());
            }
            auto colon = eat(TokenKind::Colon);
            if (!colon) {
                return std::unexpected(colon.error());
            }
            auto value_result = parse_expression_internal();
            if (!value_result) {
                return std::unexpected(value_result.error());
            }
            ast::InlineObject::Key key;
            key.kind = ast::InlineObject::KeyKind::Expression;
            key.expr_key = std::move(key_expr_result.value());
            ast::InlineObject::Entry entry;
            entry.key = std::move(key);
            entry.value = std::move(value_result.value());
            entries.push_back(std::move(entry));
        } else {
            std::string key_name;
            if (token->kind == TokenKind::LiteralString) {
                auto parsed = parse_string_literal(token->text, token->start);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                key_name = std::move(parsed.value());
                next();
            } else if (token->kind == TokenKind::Identifier) {
                key_name = token->text;
                next();
                if (peek(TokenKind::Comma) || peek(TokenKind::RCurly)) {
                    auto var = std::make_unique<ast::VariableReference>(static_cast<std::int32_t>(token->start),
                                                                        static_cast<std::int32_t>(token->end),
                                                                        key_name);
                    ast::InlineObject::Key key;
                    key.kind = ast::InlineObject::KeyKind::String;
                    key.string_key = key_name;
                    if (!string_keys.insert(key_name).second) {
                        return std::unexpected(make_error("duplicate object key", token));
                    }
                    ast::InlineObject::Entry entry;
                    entry.key = std::move(key);
                    entry.value = std::move(var);
                    entries.push_back(std::move(entry));
                    if (peek(TokenKind::Comma, true)) {
                        continue;
                    }
                    continue;
                }
            } else {
                return std::unexpected(make_error("invalid object key", token));
            }
            auto colon = eat(TokenKind::Colon);
            if (!colon) {
                return std::unexpected(colon.error());
            }
            auto value_result = parse_expression_internal();
            if (!value_result) {
                return std::unexpected(value_result.error());
            }
            if (!string_keys.insert(key_name).second) {
                return std::unexpected(make_error("duplicate object key", token));
            }
            ast::InlineObject::Key key;
            key.kind = ast::InlineObject::KeyKind::String;
            key.string_key = std::move(key_name);
            ast::InlineObject::Entry entry;
            entry.key = std::move(key);
            entry.value = std::move(value_result.value());
            entries.push_back(std::move(entry));
        }
        if (peek(TokenKind::Comma, true)) {
            continue;
        }
        break;
    }
    auto end_token = eat(TokenKind::RCurly);
    if (!end_token) {
        return std::unexpected(end_token.error());
    }
    return std::make_unique<ast::InlineObject>(static_cast<std::int32_t>(start.start),
                                               static_cast<std::int32_t>(end_token->end),
                                               std::move(entries));
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_function_or_var() {
    auto identifier_result = parse_identifier_token();
    if (!identifier_result) {
        return std::unexpected(identifier_result.error());
    }
    ast::Identifier identifier = std::move(identifier_result.value());
    if (peek(TokenKind::LParen)) {
        auto args_result = parse_method_args();
        if (!args_result) {
            return std::unexpected(args_result.error());
        }
        std::string name = identifier.name();
        Library::Function *func = library_.find_func(name);
        Library::AsyncFunction *async_func = library_.find_async_func(name);
        if (!func && !async_func) {
            ParseError error;
            error.message = "function not defined";
            error.position = static_cast<std::size_t>(identifier.start_pos());
            return std::unexpected(error);
        }
        std::int32_t start = identifier.start_pos();
        std::int32_t end = identifier.end_pos();
        if (!args_result.value().empty()) {
            end = args_result.value().back()->end_pos();
        }
        return std::make_unique<ast::FunctionCall>(start,
                                                   end,
                                                   name,
                                                   func,
                                                   async_func,
                                                   std::move(args_result.value()));
    }

    if (!identifier.name().empty() && identifier.name()[0] == '$') {
        std::size_t saved = pos_;
        if (peek(TokenKind::Dot, true)) {
            const Token *token = peek();
            if (token && token->kind == TokenKind::Identifier) {
                std::string key = token->text;
                if (identifier.name() == "$") {
                    library_.mark_root_prop(key);
                    pos_ = saved;
                } else {
                    next();
                    auto *constant = library_.find_constant(identifier.name(), key);
                    auto *async_constant = library_.find_async_constant(identifier.name(), key);
                    if (!constant && !async_constant) {
                        return std::unexpected(make_error("constant not found", token));
                    }
                    std::string full_name = identifier.name();
                    full_name.append(".");
                    full_name.append(key);
                    std::int32_t start = identifier.start_pos();
                    std::int32_t end = static_cast<std::int32_t>(token->end);
                    return std::make_unique<ast::ConstantVal>(start,
                                                              end,
                                                              std::move(full_name),
                                                              constant,
                                                              async_constant);
                }
            }
            pos_ = saved;
        }
    }

    return std::make_unique<ast::VariableReference>(identifier.start_pos(), identifier.end_pos(), identifier.name());
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_indexer(std::unique_ptr<ast::Expression> parent) {
    auto start_token = eat(TokenKind::LSquare);
    if (!start_token) {
        return std::unexpected(start_token.error());
    }
    auto expr_result = parse_expression_internal();
    if (!expr_result) {
        return std::unexpected(expr_result.error());
    }
    auto end_token = eat(TokenKind::RSquare);
    if (!end_token) {
        return std::unexpected(end_token.error());
    }
    std::int32_t start = parent->start_pos();
    std::int32_t end = static_cast<std::int32_t>(end_token->end);
    return std::make_unique<ast::Indexer>(start, end, std::move(parent), std::move(expr_result.value()));
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_dotted_node(std::unique_ptr<ast::Expression> parent) {
    auto dot = eat(TokenKind::Dot);
    if (!dot) {
        return std::unexpected(dot.error());
    }
    return parse_property(std::move(parent));
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_node(std::unique_ptr<ast::Expression> parent) {
    if (peek(TokenKind::Dot)) {
        return parse_dotted_node(std::move(parent));
    }
    if (peek(TokenKind::LSquare)) {
        return parse_indexer(std::move(parent));
    }
    return std::unexpected(ParseError{});
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_non_dotted_node(std::unique_ptr<ast::Expression> parent) {
    if (peek(TokenKind::LSquare)) {
        return parse_indexer(std::move(parent));
    }
    return std::unexpected(ParseError{});
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_property(std::unique_ptr<ast::Expression> parent) {
    if (!peek(TokenKind::Identifier)) {
        return std::unexpected(make_error("expected property name", peek()));
    }
    Token token = *next();
    std::int32_t start = parent->start_pos();
    std::int32_t end = static_cast<std::int32_t>(token.end);
    return std::make_unique<ast::PropertyReference>(start, end, token.text, std::move(parent));
}

std::expected<std::unique_ptr<ast::Expression>, ParseError> Parser::parse_function_call(ast::VariableReference &prefix) {
    std::size_t saved = pos_;
    int dot_size = 0;
    std::string name = prefix.name();
    while (peek(TokenKind::Dot, true)) {
        if (!peek(TokenKind::Identifier)) {
            pos_ = saved;
            return std::unexpected(ParseError{});
        }
        Token token = *next();
        name.append(".");
        name.append(token.text);
        ++dot_size;
        if (peek(TokenKind::LParen)) {
            auto args_result = parse_method_args();
            if (!args_result) {
                return std::unexpected(args_result.error());
            }
            Library::Function *func = library_.find_func(name);
            Library::AsyncFunction *async_func = library_.find_async_func(name);
            if (!func && !async_func && dot_size == 1) {
                auto it = directive_map_.find(prefix.name());
                if (it != directive_map_.end()) {
                    Library::DirectiveDef *def = it->second->directive_def();
                    if (def) {
                        func = def->find_func(prefix.name(), token.text);
                        async_func = def->find_async_func(prefix.name(), token.text);
                    }
                }
            }
            if (!func && !async_func) {
                return std::unexpected(make_error("function not defined", &token));
            }
            std::int32_t start = prefix.start_pos();
            std::int32_t end = static_cast<std::int32_t>(token.end);
            if (!args_result.value().empty()) {
                end = args_result.value().back()->end_pos();
            }
            return std::make_unique<ast::FunctionCall>(start,
                                                       end,
                                                       name,
                                                       func,
                                                       async_func,
                                                       std::move(args_result.value()));
        }
    }
    pos_ = saved;
    return std::unexpected(ParseError{});
}

std::expected<std::vector<std::unique_ptr<ast::Expression>>, ParseError> Parser::parse_method_args() {
    std::vector<std::unique_ptr<ast::Expression>> args;
    auto lp_result = eat(TokenKind::LParen);
    if (!lp_result) {
        return std::unexpected(lp_result.error());
    }
    if (peek(TokenKind::RParen, true)) {
        return args;
    }
    while (has_more()) {
        if (peek(TokenKind::RParen)) {
            break;
        }
        std::unique_ptr<ast::Expression> expr;
        if (peek(TokenKind::Expand)) {
            Token expand_token = *next();
            auto inner_result = parse_expression_internal();
            if (!inner_result) {
                return std::unexpected(inner_result.error());
            }
            expr = std::make_unique<ast::ExpandArrArg>(static_cast<std::int32_t>(expand_token.start),
                                                       inner_result.value()->end_pos(),
                                                       std::move(inner_result.value()),
                                                       ast::ExpandArrArg::Where::FuncCall);
        } else {
            auto expr_result = parse_expression_internal();
            if (!expr_result) {
                return std::unexpected(expr_result.error());
            }
            expr = std::move(expr_result.value());
        }
        args.push_back(std::move(expr));
        if (peek(TokenKind::Comma, true)) {
            continue;
        }
        break;
    }
    auto rp_result = eat(TokenKind::RParen);
    if (!rp_result) {
        return std::unexpected(rp_result.error());
    }
    return args;
}

std::expected<ast::Literal, ParseError> Parser::parse_literal_token(const Token &token) {
    errno = 0;
    if (token.kind == TokenKind::LiteralInt || token.kind == TokenKind::LiteralLong) {
        char *end = nullptr;
        long long value = std::strtoll(token.text.c_str(), &end, 10);
        if (end == token.text.c_str() || errno == ERANGE) {
            return std::unexpected(make_error("invalid integer literal", &token));
        }
        return ast::Literal(static_cast<std::int32_t>(token.start),
                            static_cast<std::int32_t>(token.end),
                            static_cast<std::int64_t>(value));
    }
    if (token.kind == TokenKind::LiteralHexInt || token.kind == TokenKind::LiteralHexLong) {
        char *end = nullptr;
        long long value = std::strtoll(token.text.c_str(), &end, 16);
        if (end == token.text.c_str() || errno == ERANGE) {
            return std::unexpected(make_error("invalid hex literal", &token));
        }
        return ast::Literal(static_cast<std::int32_t>(token.start),
                            static_cast<std::int32_t>(token.end),
                            static_cast<std::int64_t>(value));
    }
    if (token.kind == TokenKind::LiteralReal || token.kind == TokenKind::LiteralRealFloat) {
        char *end = nullptr;
        double value = std::strtod(token.text.c_str(), &end);
        if (end == token.text.c_str() || errno == ERANGE) {
            return std::unexpected(make_error("invalid real literal", &token));
        }
        return ast::Literal(static_cast<std::int32_t>(token.start),
                            static_cast<std::int32_t>(token.end),
                            value);
    }
    if (token.kind == TokenKind::LiteralString) {
        auto parsed = parse_string_literal(token.text, token.start);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        return ast::Literal(static_cast<std::int32_t>(token.start),
                            static_cast<std::int32_t>(token.end),
                            std::move(parsed.value()));
    }
    return std::unexpected(make_error("unsupported literal", &token));
}

std::expected<std::string, ParseError> Parser::parse_string_literal(const std::string &token_text,
                                                                    std::size_t start_pos) {
    if (token_text.size() < 2) {
        return std::unexpected(ParseError{"invalid string literal", start_pos});
    }
    char quote = token_text.front();
    if (token_text.back() != quote) {
        return std::unexpected(ParseError{"unterminated string literal", start_pos});
    }
    std::string out;
    for (std::size_t i = 1; i + 1 < token_text.size(); ++i) {
        char ch = token_text[i];
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i + 1 >= token_text.size() - 1) {
            return std::unexpected(ParseError{"unterminated escape", start_pos + i});
        }
        char esc = token_text[++i];
        switch (esc) {
            case 'a':
                out.push_back('a');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'v':
                out.push_back('\v');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '\"':
                out.push_back('\"');
                break;
            case '\'':
                out.push_back('\'');
                break;
            case 'x': {
                if (i + 2 >= token_text.size() - 1) {
                    return std::unexpected(ParseError{"invalid hex escape", start_pos + i});
                }
                unsigned value = 0;
                for (int k = 0; k < 2; ++k) {
                    char c = token_text[i + 1 + k];
                    if (!std::isxdigit(static_cast<unsigned char>(c))) {
                        return std::unexpected(ParseError{"invalid hex escape", start_pos + i});
                    }
                    value = value * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(c))
                                                                  ? c - '0'
                                                                  : std::tolower(c) - 'a' + 10);
                }
                i += 2;
                append_utf8(out, value);
                break;
            }
            case 'u': {
                if (i + 4 >= token_text.size() - 1) {
                    return std::unexpected(ParseError{"invalid unicode escape", start_pos + i});
                }
                unsigned value = 0;
                for (int k = 0; k < 4; ++k) {
                    char c = token_text[i + 1 + k];
                    if (!std::isxdigit(static_cast<unsigned char>(c))) {
                        return std::unexpected(ParseError{"invalid unicode escape", start_pos + i});
                    }
                    value = value * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(c))
                                                                  ? c - '0'
                                                                  : std::tolower(c) - 'a' + 10);
                }
                i += 4;
                append_utf8(out, value);
                break;
            }
            case '\r':
                if (i + 1 < token_text.size() - 1 && token_text[i + 1] == '\n') {
                    ++i;
                }
                break;
            case '\n':
                break;
            default:
                if (esc >= '0' && esc <= '7') {
                    unsigned value = esc - '0';
                    int count = 1;
                    while (count < 3 && i + 1 < token_text.size() - 1) {
                        char c = token_text[i + 1];
                        if (c < '0' || c > '7') {
                            break;
                        }
                        value = value * 8 + static_cast<unsigned>(c - '0');
                        ++i;
                        ++count;
                    }
                    append_utf8(out, value);
                } else {
                    return std::unexpected(ParseError{"invalid escape", start_pos + i});
                }
        }
    }
    return out;
}

std::expected<ast::Identifier, ParseError> Parser::parse_identifier_token() {
    auto token_result = eat(TokenKind::Identifier);
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    if (is_keyword(token_result->text)) {
        return std::unexpected(make_error("keyword not expected", &token_result.value()));
    }
    return ast::Identifier(static_cast<std::int32_t>(token_result->start),
                           static_cast<std::int32_t>(token_result->end),
                           token_result->text);
}

bool Parser::has_more() const {
    return pos_ < tokens_.size();
}

const Token *Parser::peek() const {
    if (!has_more()) {
        return nullptr;
    }
    return &tokens_[pos_];
}

const Token *Parser::next() {
    if (!has_more()) {
        return nullptr;
    }
    return &tokens_[pos_++];
}

bool Parser::peek(TokenKind kind, bool consume) {
    if (!has_more()) {
        return false;
    }
    if (tokens_[pos_].kind == kind) {
        if (consume) {
            ++pos_;
        }
        return true;
    }
    return false;
}

bool Parser::peek(TokenKind possible1, TokenKind possible2) {
    if (!has_more()) {
        return false;
    }
    TokenKind kind = tokens_[pos_].kind;
    return kind == possible1 || kind == possible2;
}

bool Parser::peek(TokenKind possible1, TokenKind possible2, TokenKind possible3) {
    if (!has_more()) {
        return false;
    }
    TokenKind kind = tokens_[pos_].kind;
    return kind == possible1 || kind == possible2 || kind == possible3;
}

bool Parser::peek_identifier(std::string_view identifier) const {
    if (!has_more()) {
        return false;
    }
    const Token &token = tokens_[pos_];
    return token.kind == TokenKind::Identifier && token.text == identifier;
}

std::expected<Token, ParseError> Parser::eat(TokenKind expected_kind) {
    if (!has_more()) {
        return std::unexpected(make_error("unexpected end of input", nullptr));
    }
    Token token = tokens_[pos_++];
    if (token.kind != expected_kind) {
        return std::unexpected(make_error("unexpected token", &token));
    }
    return token;
}

std::expected<Token, ParseError> Parser::eat_keyword(std::string_view keyword) {
    auto token_result = eat(TokenKind::Identifier);
    if (!token_result) {
        return std::unexpected(token_result.error());
    }
    if (token_result->text != keyword) {
        return std::unexpected(make_error("keyword not matched", &token_result.value()));
    }
    return token_result;
}

ParseError Parser::make_error(const std::string &message, const Token *token) const {
    ParseError error;
    if (message.empty()) {
        return error;
    }
    error.message = message;
    if (token) {
        error.position = token->start;
    }
    return error;
}

} // namespace fiber::script::parse
