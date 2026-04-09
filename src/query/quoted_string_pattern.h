#pragma once

#include "query/ast.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace pando {

// Same character set as CWB dialect (cwb_parser.cpp): if absent, treat as literal for "=".
inline bool string_has_regex_metachar(std::string_view s) {
    for (char ch : s) {
        switch (ch) {
        case '.':
        case '*':
        case '+':
        case '?':
        case '[':
        case ']':
        case '(':
        case ')':
        case '{':
        case '}':
        case '|':
        case '^':
        case '$':
        case '\\':
            return true;
        default:
            break;
        }
    }
    return false;
}

// Native `[attr = "value"]` and bare-token `"value"` / `form` shorthand: when strict_quoted_strings
// is false, behave like CWB/Manatee — quoted "=" uses whole-token regex if value looks like a pattern.
// The executor applies RE2::FullMatch / std::regex_match (regex_full_match), not literal ^$ in the pattern.
inline void interpret_quoted_eq_string(AttrCondition& ac, std::string raw,
                                     bool strict_quoted_strings) {
    ac.regex_full_match = false;
    if (strict_quoted_strings) {
        ac.op = CompOp::EQ;
        ac.value = std::move(raw);
        return;
    }
    if (!string_has_regex_metachar(raw)) {
        ac.op = CompOp::EQ;
        ac.value = std::move(raw);
        return;
    }
    ac.op = CompOp::REGEX;
    ac.regex_full_match = true;
    ac.value = std::move(raw);
}

inline void validate_neq_quoted_string(const std::string& value, bool strict_quoted_strings) {
    if (strict_quoted_strings)
        return;
    if (string_has_regex_metachar(value))
        throw std::runtime_error(
            "Unsupported: != with quoted string that contains regex metacharacters (use == with "
            "literal or /pattern/, or pass --strict-quoted-strings for literal-only quotes)");
}

} // namespace pando
