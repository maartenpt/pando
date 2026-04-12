#include "query/parser.h"

int main() {
    using pando::Parser;
    using pando::ParserOptions;
    Parser(R"([form = "th.*"])", {}).parse();
    Parser(R"([form = "the"])", {}).parse();
    ParserOptions strict;
    strict.strict_quoted_strings = true;
    Parser(R"([form = "th.*"])", strict).parse();
    // UD-style feats: slash form (feats/Key) is a single identifier; same as feats.Key after normalize
    Parser(R"([feats/Definite="Ind"])", {}).parse();
    Parser(R"([feats.Number="Sing"])", {}).parse();
    Parser(R"([form=/foo/])", {}).parse();
    return 0;
}
