#include "query/dialect/cwb/cwb_translate.h"

#include <stdexcept>
#include <string>

namespace manatree {

Program translate_cwb_program(const std::string&, int, std::string*) {
    throw std::runtime_error(
        "The CWB/CQP dialect was not built in this binary. "
        "Configure with -DPANDO_CWB_DIALECT=ON (see dev/CQL-DIALECT-LICENSING.md).");
}

} // namespace manatree
