#pragma once

#include "query/ast.h"

#include <string>
#include <string_view>

namespace pando::pmltq {

// Shared by gold JSON and native parser: map PML-TQ field / operator / raw value to
// a leaf AttrCondition (CWB-aligned = vs regex metacharacters).
ConditionPtr lower_pmltq_test_strings(const std::string& field, const std::string& op,
                                      const std::string& value, std::string& err);

std::string pmltq_map_field(std::string field);

} // namespace pando::pmltq
