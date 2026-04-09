#pragma once

#include "query/ast.h"

#include <string>

namespace pando::pmltq {

struct JsonValue;

// Lower gold JSON `ast` (PEG/ClickPMLTQ) to native pando Program. Growing subset:
// single selector, type selector/a-node, flat and/tests, no >> return yet.
Program lower_gold_ast_to_program(const JsonValue& gold_response, std::string& err);

} // namespace pando::pmltq
