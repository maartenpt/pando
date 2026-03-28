#include "query/dialect/pmltq/pmltq_translate.h"

#include <stdexcept>
#include <string>

namespace manatree {

Program translate_pmltq_program(const std::string&, int, std::string*) {
    throw std::runtime_error(
        "The PML-TQ dialect was not built in this binary. "
        "Configure with -DPANDO_PMLTQ_DIALECT=ON (see dev/PMLTQ-ROADMAP.md).");
}

bool translate_pmltq_export_click_sql(const std::string&, std::string*, std::string* err_msg) {
    if (err_msg)
        *err_msg = "PML-TQ dialect was not built in this binary.";
    return false;
}

} // namespace manatree
