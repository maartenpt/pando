#pragma once

#include "query/ast.h"
#include <string>

namespace manatree {

/// Parse CWB/CQP-style input using a lexer aligned with IMS CWB parser.l / parser.y.
/// Throws std::runtime_error on syntax errors or unsupported constructs.
Program translate_cwb_program_parsed(const std::string& input, int debug_level,
                                       std::string* trace_out);

} // namespace manatree
