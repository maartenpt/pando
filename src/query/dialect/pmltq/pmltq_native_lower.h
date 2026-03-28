#pragma once

#include "query/ast.h"

#include "query/dialect/pmltq/pmltq_ast.h"

#include <string>

namespace manatree::pmltq {

Program lower_native_pmltq_query(const PmltqQuery& q, std::string& err);

} // namespace manatree::pmltq
