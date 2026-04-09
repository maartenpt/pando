#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pando::cwb {

/// Token kinds aligned with IMS CWB cqp/parser.l / parser.y (clean-room).
enum class CwbTok {
    END = 0,

    // Punctuation / operators (single-char and multi-char)
    LPAREN,
    RPAREN,
    LBRACK,
    RBRACK,
    LBRACE,
    RBRACE,
    SEMI,
    COMMA,
    COLON,
    PIPE,
    AMP,
    BANG,
    STAR,
    PLUS,
    QUEST,
    EQ,
    NEQ,
    LT,
    GT,
    LET,
    GET,
    MINUS,
    TILDE,
    DOT,
    IMPLIES,      // ->
    GCDEL,        // ::
    ELLIPSIS,     // .. or ...
    APPEND,       // >>
    LEFT_APPEND,  // <<
    PLUSEQ,
    MINUSEQ,
    RE_PAREN,     // RE(
    EXTENSION,    // (?
    ESCAPE,       // `
    AT,
    SLASH,

    // Literals / lexer-only
    ID,
    STRING,
    INTEGER,
    DOUBLE,
    FLAG,       // %[a-z]+ after strip
    VARIABLE,   // $name
    LABEL,      // id:  (generic label before a token pattern)
    FIELDLABEL, // match: matchend: target: collocate: keyword:
    QID,        // id.id
    NQRID,      // named query id forms — rarely in patterns
    IPADDRESS,
    IPSUBNET,

    TAGSTART,     // <id  (XML open, partial)
    TAGEND,       // </id>
    ANCHORTAG,    // <match> etc.
    ANCHORENDTAG, // </match> etc.
    MATCHALL,     // []
    LCMATCHALL,   // [::]
    LCSTART,      // [:
    LCEND,        // :]  (lookahead constraint close)

    // Bare FIELD words (match/matchend/... without trailing ':')
    FIELD,

    // CQP command / reserved words (lexer returns these instead of ID)
    EXIT_SYM,
    CAT_SYM,
    DEFINE_SYM,
    DIFF_SYM,
    DISCARD_SYM,
    INTER_SYM,
    JOIN_SYM,
    SUBSET_SYM,
    LEFT_SYM,
    RIGHT_SYM,
    SAVE_SYM,
    SHOW_SYM,
    CD_SYM,
    GROUP_SYM,
    WHERE_SYM,
    WITHIN_SYM,
    WITH_SYM,
    WITHOUT_SYM,
    DELETE_SYM,
    EXPAND_SYM,
    TO_SYM,
    SET_SYM,
    EXEC_SYM,
    CUT_SYM,
    INFO_SYM,
    MEET_SYM,
    UNION_SYM,
    MU_SYM,
    TAB_SYM,
    SORT_SYM,
    COUNT_SYM,
    BY_SYM,
    FOREACH_SYM,
    ON_SYM,
    YES_SYM,
    OFF_SYM,
    NO_SYM,
    ASC_SYM,
    DESC_SYM,
    REVERSE_SYM,
    SLEEP_SYM,
    REDUCE_SYM,
    MAXIMAL_SYM,
    SIZE_SYM,
    DUMP_SYM,
    UNDUMP_SYM,
    TABULATE_SYM,
    NOT_SYM,
    CONTAINS_SYM,
    MATCHES_SYM,
    UNLOCK_SYM,
    USER_SYM,
    HOST_SYM,
    MACRO_SYM,
    RANDOMIZE_SYM,
    FROM_SYM,
    INCLUSIVE_SYM,
    EXCLUSIVE_SYM,
    NULL_SYM,
    EOL_SYM,  // .EOL.
};

struct CwbToken {
    CwbTok kind = CwbTok::END;
    std::string text;   // identifier, string body, label without ':', etc.
    int ival = 0;
    double fval = 0;
    std::size_t offset = 0;  // start offset in source
};

/// Flex-compatible lexer for the CWB query subset (no CWB code copied).
class CwbLexer {
public:
    explicit CwbLexer(std::string_view src) : s_(src) {}

    CwbToken next();

    std::size_t pos() const { return i_; }

private:
    std::string_view s_;
    std::size_t i_ = 0;

    void skip_ws_and_comments();
    char peek_ch(std::size_t j = 0) const;
    void bump_ch(std::size_t n = 1);
    CwbToken make_tok(CwbTok k, std::size_t start, std::string t = {});
    std::string read_string_double();
    std::string read_string_single();
    static std::string strip_quotes(std::string_view raw);
};

} // namespace pando::cwb
