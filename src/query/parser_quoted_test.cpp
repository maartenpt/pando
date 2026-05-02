#include "query/parser.h"
#include "query/ast.h"
#include <cassert>

int main() {
    using pando::CommandType;
    using pando::Parser;
    using pando::ParserOptions;
    Parser(R"([form = "th.*"])", {}).parse();
    Parser(R"([form = "the"])", {}).parse();
    ParserOptions strict;
    strict.strict_quoted_strings = true;
    Parser(R"([form = "th.*"])", strict).parse();
    // UD-style feats: feats/Key only (dot form feats.Key is rejected; '.' is name.attr).
    Parser(R"([feats/Definite="Ind"])", {}).parse();
    Parser(R"([feats/Number="Sing"])", {}).parse();
    Parser(R"([form=/foo/])", {}).parse();

    // dep_subtree: inline chain + global tcnt
    Parser(
            R"(barenoun:[upos="NOUN"] longbarenp:dep_subtree(barenoun) :: tcnt(longbarenp) > 2)",
            {})
            .parse();
    Parser(R"(longbarenp = dep_subtree(barenoun) :: tcnt(longbarenp) > 2)", {}).parse();
    Parser(R"(a:[]; stats avg(strlen(a.form)), median(strlen(a.form)) by a.text_id)", {}).parse();

    // coll / dcoll: parenthesized match-set name and (on label)
    {
        auto prog = Parser(R"(coll (MySet) (on hub) by lemma)", {}).parse();
        assert(prog.size() == 1 && prog[0].has_command);
        assert(prog[0].command.type == CommandType::COLL);
        assert(prog[0].command.query_name == "MySet");
        assert(prog[0].command.coll_on_label == "hub");
    }
    {
        auto prog = Parser(R"(coll on b by lemma)", {}).parse();
        assert(prog[0].command.query_name.empty());
        assert(prog[0].command.coll_on_label == "b");
    }
    {
        auto prog = Parser(R"(coll LastQuery on b by lemma)", {}).parse();
        assert(prog[0].command.query_name == "LastQuery");
        assert(prog[0].command.coll_on_label == "b");
    }
    {
        auto prog = Parser(R"(dcoll (Saved) a.amod by lemma)", {}).parse();
        assert(prog.size() == 1 && prog[0].has_command);
        assert(prog[0].command.type == CommandType::DCOLL);
        assert(prog[0].command.query_name == "Saved");
        assert(prog[0].command.dcoll_anchor == "a");
        assert(prog[0].command.relations.size() == 1 && prog[0].command.relations[0] == "amod");
    }
    {
        auto prog = Parser(R"(dcoll (on anchor1) by lemma)", {}).parse();
        assert(prog[0].command.dcoll_anchor == "anchor1");
        assert(prog[0].command.relations.empty());
    }
    return 0;
}
