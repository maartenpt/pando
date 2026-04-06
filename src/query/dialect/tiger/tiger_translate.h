#pragma once

#include "query/ast.h"
#include "query/parser.h"
#include <string>

namespace manatree {

// Thin TIGERSearch-style surface → native ClickCQL (constituency / nested `node` regions).
// Expands line-based macros, then parses with the normal Parser. See dev/TIGER-DIALECT.md.
// Throws std::runtime_error on unknown lines or parse errors.
Program translate_tiger_program(const std::string& input, int debug_level,
                                std::string* trace_out,
                                ParserOptions parse_opts = {});

} // namespace manatree
