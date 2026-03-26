#pragma once

#include "query/ast.h"
#include "query/lexer.h"
#include <string>

namespace manatree {

// Recursive-descent parser for ClickCQL.
//
// Parses semicolon-separated statements.  Each statement is either:
//   - A token query (possibly named)
//   - A grouping command (count, group, sort, freq, coll, dcoll, cat, size, ...)
//
// The parser handles the ambiguity between dependency operators (> <) and
// comparison operators by context: inside [] they are comparison, outside
// they are dependency relations.
class Parser {
public:
    explicit Parser(const std::string& input);

    Program parse();

private:
    Statement parse_statement();

    // Check if this is a command keyword (count, size, show, etc.)
    bool is_command_keyword(const std::string& text) const;
    GroupCommand parse_command();

    TokenQuery  parse_token_query();
    QueryToken  parse_token_expr();
    ConditionPtr parse_conditions();
    ConditionPtr parse_or_condition();
    ConditionPtr parse_and_condition();
    ConditionPtr parse_primary_condition();

    // Parse optional repetition quantifier after a token: {n,m}, {n,}, {n}, +, ?
    void parse_repetition(QueryToken& qt);

    // Try to parse a relation operator between tokens.
    // Returns true and fills `rel` if a relation is found.
    bool try_parse_relation(QueryRelation& rel);

    // Parse the within clause: "within s", "within s_tuid='X'", "within s having [cond]"
    void parse_within_clause(TokenQuery& tq);

    // Parse "containing s", "containing subtree [cond]"
    void parse_containing_clause(TokenQuery& tq, bool negated);

    // #12: Parse :: global_region_filter and/or global_alignment_filter [ & ... ]*
    void parse_global_filters(TokenQuery& tq);

    Lexer lexer_;
};

} // namespace manatree
