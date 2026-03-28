
#include "query/dialect/cwb/cwb_translate.h"
#include "query/dialect/cwb/cwb_parser.h"

namespace manatree {

Program translate_cwb_program(const std::string& input, int debug_level,
                              std::string* trace_out) {
    return translate_cwb_program_parsed(input, debug_level, trace_out);
}

} // namespace manatree
