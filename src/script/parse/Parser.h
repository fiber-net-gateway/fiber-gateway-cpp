#ifndef FIBER_SCRIPT_PARSE_PARSER_H
#define FIBER_SCRIPT_PARSE_PARSER_H

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Tokenizer.h"
#include "ParseError.h"
#include "../ast/Assign.h"
#include "../ast/Block.h"
#include "../ast/BinaryOperator.h"
#include "../ast/BreakStatement.h"
#include "../ast/ConstantVal.h"
#include "../ast/ContinueStatement.h"
#include "../ast/DirectiveStatement.h"
#include "../ast/ExpandArrArg.h"
#include "../ast/Expression.h"
#include "../ast/ExpressionStatement.h"
#include "../ast/ForeachStatement.h"
#include "../ast/FunctionCall.h"
#include "../ast/Identifier.h"
#include "../ast/IfStatement.h"
#include "../ast/InlineList.h"
#include "../ast/InlineObject.h"
#include "../ast/Indexer.h"
#include "../ast/LogicRelationalExpression.h"
#include "../ast/Literal.h"
#include "../ast/MaybeLValue.h"
#include "../ast/PropertyReference.h"
#include "../ast/ReturnStatement.h"
#include "../ast/Ternary.h"
#include "../ast/ThrowStatement.h"
#include "../ast/TryCatchStatement.h"
#include "../ast/UnaryOperator.h"
#include "../ast/VariableDeclareStatement.h"
#include "../ast/VariableReference.h"

namespace fiber::script {
class Library;
}

namespace fiber::script::parse {

class Parser {
public:
    Parser(Library &library, bool allow_assign);

    std::expected<std::unique_ptr<ast::Block>, ParseError> parse_script(std::string_view script);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_expression(std::string_view expression);

private:
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_break_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_continue_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_return_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_throw_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_try_catch_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_if_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_foreach_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_variable_declare_statement();
    std::expected<std::unique_ptr<ast::Statement>, ParseError> parse_directive_statement();
    std::expected<std::unique_ptr<ast::Block>, ParseError> parse_block(bool must_curly, ast::BlockType type);

    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_expression_internal();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_logical_or();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_logical_and();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_relational();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_sum();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_product();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_unary();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_primary();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_start_node();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_paren_expression();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_literal();
    std::expected<std::optional<ast::Literal>, ParseError> parse_optional_literal();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_inline_list();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_inline_object();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_function_or_var();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_indexer(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_dotted_node(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_node(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_non_dotted_node(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_property(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_function_call(ast::VariableReference &prefix);
    std::expected<std::vector<std::unique_ptr<ast::Expression>>, ParseError> parse_method_args();

    std::expected<ast::Literal, ParseError> parse_literal_token(const Token &token);
    std::expected<std::string, ParseError> parse_string_literal(const std::string &token_text, std::size_t start_pos);
    std::expected<ast::Identifier, ParseError> parse_identifier_token();

    bool has_more() const;
    const Token *peek() const;
    const Token *next();
    bool peek(TokenKind kind, bool consume = false);
    bool peek(TokenKind possible1, TokenKind possible2);
    bool peek(TokenKind possible1, TokenKind possible2, TokenKind possible3);
    bool peek_identifier(std::string_view identifier) const;
    std::expected<Token, ParseError> eat(TokenKind expected_kind);
    std::expected<Token, ParseError> eat_keyword(std::string_view keyword);

    ParseError make_error(const std::string &message, const Token *token) const;

    Library &library_;
    bool allow_assign_ = true;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::unordered_map<std::string, ast::DirectiveStatement *> directive_map_;
    std::vector<std::unique_ptr<ast::DirectiveStatement>> directive_statements_;
};

} // namespace fiber::script::parse

#endif // FIBER_SCRIPT_PARSE_PARSER_H
