#pragma once

#include "query/dialect/pmltq/pmltq_ast.h"

#include <string>

namespace manatree::pmltq {

// Native recursive-descent parser (Grammar.pm subset). Throws std::runtime_error on
// syntax errors or constructs that require the gold bridge (e.g. >>).
PmltqQuery parse_pmltq_query(const std::string& input);

} // namespace manatree::pmltq
