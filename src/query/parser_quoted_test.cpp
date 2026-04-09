#include "query/parser.h"

int main() {
    using pando::Parser;
    using pando::ParserOptions;
    Parser(R"([form = "th.*"])", {}).parse();
    Parser(R"([form = "the"])", {}).parse();
    ParserOptions strict;
    strict.strict_quoted_strings = true;
    Parser(R"([form = "th.*"])", strict).parse();
    return 0;
}
