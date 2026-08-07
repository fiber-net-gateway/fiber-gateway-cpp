#ifndef FIBER_SCRIPT_PARSE_PARSER_H
#define FIBER_SCRIPT_PARSE_PARSER_H

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../ast/Assign.h"
#include "../ast/BinaryOperator.h"
#include "../ast/Block.h"
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
#include "../ast/Indexer.h"
#include "../ast/InlineList.h"
#include "../ast/InlineObject.h"
#include "../ast/Literal.h"
#include "../ast/LogicRelationalExpression.h"
#include "../ast/MaybeLValue.h"
#include "../ast/Node.h"
#include "../ast/PropertyReference.h"
#include "../ast/ReturnStatement.h"
#include "../ast/TemplateString.h"
#include "../ast/Ternary.h"
#include "../ast/ThrowStatement.h"
#include "../ast/TryCatchStatement.h"
#include "../ast/UnaryOperator.h"
#include "../ast/VariableDeclareStatement.h"
#include "../ast/VariableReference.h"
#include "ParseError.h"
#include "Tokenizer.h"

namespace fiber::script {
class Library;
}

namespace fiber::script::parse {

class Parser {
public:
    Parser(Library &library, bool allow_assign, std::size_t max_depth = kDefaultScriptMaxDepth);

    std::expected<std::unique_ptr<ast::Block>, ParseError> parse_script(std::string_view script);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_expression(std::string_view expression);

    // Parses a template-literal BODY (without surrounding backticks) into a TemplateString
    // expression. The body's ${...} interpolations, escapes, and nested expressions are handled
    // by the existing template-literal parser (the body is wrapped in backticks internally and
    // run through parse_expression). allow_assign/max_depth are inherited from this Parser, so
    // they propagate to the interpolated expressions as well.
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_template(std::string_view template_string);

private:
    class DepthGuard {
    public:
        DepthGuard() noexcept = default;
        explicit DepthGuard(Parser &parser) noexcept;
        DepthGuard(const DepthGuard &) = delete;
        DepthGuard &operator=(const DepthGuard &) = delete;
        DepthGuard(DepthGuard &&other) noexcept;
        DepthGuard &operator=(DepthGuard &&other) noexcept;
        ~DepthGuard();

    private:
        Parser *parser_ = nullptr;
    };

    struct ResolvedFunctionCall {
        const Library::HostCallable *func = nullptr;
        const Library::HostCallable *async_func = nullptr;
        std::vector<fiber::script::JsValue> default_args;
    };

    Parser(Library &library, bool allow_assign, std::size_t max_depth, std::size_t parse_depth);

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
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_template_literal();
    std::expected<std::optional<ast::Literal>, ParseError> parse_optional_literal();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_inline_list();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_inline_object();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_function_or_var();
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_indexer(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError>
    parse_dotted_node(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_node(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError>
    parse_non_dotted_node(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_property(std::unique_ptr<ast::Expression> parent);
    std::expected<std::unique_ptr<ast::Expression>, ParseError> parse_function_call(ast::VariableReference &prefix);
    std::expected<std::vector<std::unique_ptr<ast::Expression>>, ParseError> parse_method_args();
    std::expected<Library::FunctionMatchRequest, ParseError>
    make_function_match_request(const std::vector<std::unique_ptr<ast::Expression>> &args,
                                const Token *error_token) const;
    std::expected<ResolvedFunctionCall, ParseError>
    resolve_function_call(std::string_view name, const std::vector<std::unique_ptr<ast::Expression>> &args,
                          const Token *error_token) const;

    std::expected<ast::Literal, ParseError> parse_literal_token(const Token &token);
    std::expected<std::string, ParseError> parse_string_literal(const std::string &token_text, std::size_t start_pos);
    std::expected<std::string, ParseError> parse_template_chunk(std::string_view token_text, std::size_t start_pos);
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

    std::expected<DepthGuard, ParseError> enter_depth(const Token *token);
    std::expected<void, ParseError> check_depth_slot(std::size_t depth, const Token *token) const;
    ParseError make_depth_error(const Token *token) const;
    ParseError make_error(const std::string &message, const Token *token) const;

    Library &library_;
    std::size_t max_depth_ = 0;
    bool allow_assign_ = true;
    std::size_t parse_depth_ = 0;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::unordered_map<std::string, ast::DirectiveStatement *> directive_map_;
    std::vector<std::unique_ptr<ast::DirectiveStatement>> directive_statements_;
};

} // namespace fiber::script::parse

#endif // FIBER_SCRIPT_PARSE_PARSER_H
