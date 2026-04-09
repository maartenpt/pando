#pragma once

#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace pando {

// JSON hierarchy for count/group by multiple fields: first field = outer layer,
// nested children for each subsequent field (sorted by count descending at each level).
void emit_count_result_hierarchy_json(std::ostream& out,
                                      const std::vector<std::string>& fields,
                                      const std::map<std::string, size_t>& counts,
                                      size_t total,
                                      size_t group_limit);

} // namespace pando
