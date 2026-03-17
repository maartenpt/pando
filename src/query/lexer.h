#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace manatree {

enum class TokType {
    END,
    LBRACKET,    // [
    RBRACKET,    // ]
    LPAREN,      // (
    RPAREN,      // )
    EQ,          // =
    NEQ,         // !=
    LT,          // <
    GT,          // >
    LTE,         // <=
    GTE,         // >=
    LTLT,        // <<
    GTGT,        // >>
    BANG_LT,     // !<
    BANG_GT,     // !>
    AMP,         // &
    PIPE,        // |
    STRING,      // "..."
    REGEX,       // /pattern/
    IDENT,       // identifier
    NUMBER,      // integer
    COLON,       // :
    DCOLON,      // ::
    AT,          // @
    SEMI,        // ;
    DOT,         // .
    COMMA,       // ,
    LBRACE,      // {
    RBRACE,      // }
    PLUS,        // +
    QUESTION,    // ?
};

struct Token {
    TokType     type = TokType::END;
    std::string text;
    size_t      pos = 0;    // character position in input
};

class Lexer {
public:
    explicit Lexer(const std::string& input);

    Token next();
    Token peek();
    void  consume();
    bool  at_end() const;

    // Consume and verify the expected token type. Throws on mismatch.
    Token expect(TokType type);

private:
    void skip_whitespace();

    std::string input_;
    size_t      pos_ = 0;
    Token       lookahead_;
    bool        has_lookahead_ = false;
};

} // namespace manatree
