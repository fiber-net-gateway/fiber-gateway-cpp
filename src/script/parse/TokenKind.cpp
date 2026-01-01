#include "TokenKind.h"

namespace fiber::script::parse {

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::LiteralInt:
            return "LiteralInt";
        case TokenKind::LiteralLong:
            return "LiteralLong";
        case TokenKind::LiteralHexInt:
            return "LiteralHexInt";
        case TokenKind::LiteralHexLong:
            return "LiteralHexLong";
        case TokenKind::LiteralString:
            return "LiteralString";
        case TokenKind::LiteralReal:
            return "LiteralReal";
        case TokenKind::LiteralRealFloat:
            return "LiteralRealFloat";
        case TokenKind::LParen:
            return "LParen";
        case TokenKind::RParen:
            return "RParen";
        case TokenKind::Comma:
            return "Comma";
        case TokenKind::Identifier:
            return "Identifier";
        case TokenKind::Colon:
            return "Colon";
        case TokenKind::Hash:
            return "Hash";
        case TokenKind::RSquare:
            return "RSquare";
        case TokenKind::LSquare:
            return "LSquare";
        case TokenKind::LCurly:
            return "LCurly";
        case TokenKind::RCurly:
            return "RCurly";
        case TokenKind::Expand:
            return "Expand";
        case TokenKind::Dot:
            return "Dot";
        case TokenKind::Plus:
            return "Plus";
        case TokenKind::Star:
            return "Star";
        case TokenKind::Minus:
            return "Minus";
        case TokenKind::QMark:
            return "QMark";
        case TokenKind::Project:
            return "Project";
        case TokenKind::Div:
            return "Div";
        case TokenKind::GE:
            return "GE";
        case TokenKind::GT:
            return "GT";
        case TokenKind::LE:
            return "LE";
        case TokenKind::LT:
            return "LT";
        case TokenKind::Seq:
            return "Seq";
        case TokenKind::Sne:
            return "Sne";
        case TokenKind::Eq:
            return "Eq";
        case TokenKind::Ne:
            return "Ne";
        case TokenKind::Mod:
            return "Mod";
        case TokenKind::Semicolon:
            return "Semicolon";
        case TokenKind::Not:
            return "Not";
        case TokenKind::Assign:
            return "Assign";
        case TokenKind::Tilde:
            return "Tilde";
        case TokenKind::Select:
            return "Select";
        case TokenKind::SymbolicOr:
            return "SymbolicOr";
        case TokenKind::SymbolicAnd:
            return "SymbolicAnd";
        case TokenKind::Inc:
            return "Inc";
        case TokenKind::Dec:
            return "Dec";
    }
    return "Unknown";
}

} // namespace fiber::script::parse
