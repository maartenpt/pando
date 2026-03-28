#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manatree::pmltq {

// Token kinds for PML-TQ surface syntax (aligned with perl-pmltq Grammar.pm; keywords
// are not folded here — the parser treats IDENT vs reserved words).
enum class PmltqTok {
    END,

    IDENT,
    STRING,
    NUMBER,

    // Multi-char operators (longest-match in lexer)
    GTGT,     // >>
    COLON_EQ, // :=
    GTE,      // >=
    LTE,      // <=
    NEQ,      // !=
    EQEQ,     // ==
    ARROW,    // ->
    DOTDOT,   // ..

    // Single-char / punctuation
    EQ, LT, GT, BANG, MINUS, PIPE, AMP, PLUS, STAR, QMARK, COLON, SEMI, COMMA, DOT,
    LPAREN, RPAREN, LBRACKET, RBRACKET, LBRACE, RBRACE, SLASH, BACKSLASH,
    DOLLAR, AT, CARET, TILDE, PERCENT,

    // Invalid / unrecognised (error reporting)
    INVALID,
};

struct PmltqToken {
    PmltqTok type = PmltqTok::END;
    std::string text; // lexeme (IDENT, STRING, NUMBER) or operator text
    size_t byte_offset = 0; // start of token after skip
};

// Lexer: inter-token skip matches Grammar.pm `$skip`: `\s*` and `#...\n` chunks.
class PmltqLexer {
public:
    explicit PmltqLexer(std::string_view input);

    PmltqToken peek();
    PmltqToken consume();

private:
    void skip_pre_token();
    PmltqToken read_next();

    std::string_view s_;
    size_t i_ = 0;
    std::optional<PmltqToken> peeked_;
};

// Exposed for tests and future parser layers.
std::vector<PmltqToken> tokenize_pmltq_all(std::string_view input);

} // namespace manatree::pmltq
