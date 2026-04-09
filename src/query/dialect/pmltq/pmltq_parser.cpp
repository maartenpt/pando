#include "query/dialect/pmltq/pmltq_parser.h"

#include "query/dialect/pmltq/pmltq_lexer.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace pando::pmltq {

namespace {

class PmltqParser {
public:
    explicit PmltqParser(std::string_view input) : lx_(input) { cur_ = lx_.consume(); }

    PmltqQuery parse_query() {
        if (cur_.type == PmltqTok::END)
            throw std::runtime_error("PMLTQ: empty query (no selectors after comments/whitespace)");
        if (cur_.type == PmltqTok::INVALID) {
            std::ostringstream o;
            o << "PMLTQ: invalid character at byte " << cur_.byte_offset;
            throw std::runtime_error(o.str());
        }

        PmltqQuery q;
        q.selector = parse_selector();
        while (cur_.type == PmltqTok::SEMI)
            bump();

        if (cur_.type == PmltqTok::GTGT) {
            bump();
            q.has_output = true;
            q.output = parse_output_block();
        }

        while (cur_.type == PmltqTok::SEMI)
            bump();

        if (cur_.type != PmltqTok::END) {
            std::ostringstream o;
            o << "PMLTQ: unexpected token (native parser: one selector and optional >> block; "
                 "set PMLTQ_GOLD_JS_DIR for other constructs) at byte "
              << cur_.byte_offset;
            throw std::runtime_error(o.str());
        }
        return q;
    }

private:
    PmltqLexer lx_;
    PmltqToken cur_;

    void bump() { cur_ = lx_.consume(); }

    [[noreturn]] void throw_expected(const char* what) {
        std::ostringstream o;
        o << "PMLTQ: expected " << what << " at byte " << cur_.byte_offset;
        throw std::runtime_error(o.str());
    }

    void expect(PmltqTok t) {
        if (cur_.type != t)
            throw_expected("different token");
    }

    PmltqSelector parse_selector() {
        PmltqSelector s;
        if (cur_.type == PmltqTok::LBRACKET) {
            s.kind = PmltqSelectorKind::Tok;
            bump();
            if (cur_.type == PmltqTok::RBRACKET) {
                bump();
                s.has_cond = false;
                return s;
            }
            s.has_cond = true;
            s.cond = parse_or_expr();
            expect(PmltqTok::RBRACKET);
            bump();
            return s;
        }

        if (cur_.type != PmltqTok::IDENT)
            throw_expected("selector (a-node, a-root, tok, or '[')");

        const std::string kw = cur_.text;
        if (kw == "a-node")
            s.kind = PmltqSelectorKind::ANode;
        else if (kw == "a-root")
            s.kind = PmltqSelectorKind::ARoot;
        else if (kw == "tok")
            s.kind = PmltqSelectorKind::Tok;
        else {
            std::ostringstream o;
            o << "PMLTQ: unknown selector '" << kw << "' at byte " << cur_.byte_offset;
            throw std::runtime_error(o.str());
        }
        bump();

        if (cur_.type == PmltqTok::DOLLAR) {
            bump();
            if (cur_.type != PmltqTok::IDENT)
                throw_expected("variable name after '$'");
            s.bind_name = cur_.text;
            bump();
            if (cur_.type != PmltqTok::COLON_EQ)
                throw_expected("':=' after binding");
            bump();
        }

        if (cur_.type != PmltqTok::LBRACKET)
            throw_expected("'['");
        bump();

        if (cur_.type == PmltqTok::RBRACKET) {
            bump();
            s.has_cond = false;
            return s;
        }

        s.has_cond = true;
        s.cond = parse_or_expr();
        expect(PmltqTok::RBRACKET);
        bump();
        return s;
    }

    PmltqCondExpr parse_or_expr() {
        PmltqCondExpr left = parse_and_expr();
        while (cur_.type == PmltqTok::IDENT && cur_.text == "or") {
            bump();
            PmltqCondExpr r = parse_and_expr();
            if (left.kind == PmltqCondKind::Or) {
                left.children.push_back(std::move(r));
            } else {
                PmltqCondExpr comb;
                comb.kind = PmltqCondKind::Or;
                comb.children.push_back(std::move(left));
                comb.children.push_back(std::move(r));
                left = std::move(comb);
            }
        }
        return left;
    }

    PmltqCondExpr parse_and_expr() {
        PmltqCondExpr left = parse_primary();
        while (cur_.type == PmltqTok::IDENT && cur_.text == "and") {
            bump();
            PmltqCondExpr r = parse_primary();
            if (left.kind == PmltqCondKind::And) {
                left.children.push_back(std::move(r));
            } else {
                PmltqCondExpr comb;
                comb.kind = PmltqCondKind::And;
                comb.children.push_back(std::move(left));
                comb.children.push_back(std::move(r));
                left = std::move(comb);
            }
        }
        return left;
    }

    PmltqCondExpr parse_primary() {
        if (cur_.type == PmltqTok::LPAREN) {
            bump();
            PmltqCondExpr e = parse_or_expr();
            expect(PmltqTok::RPAREN);
            bump();
            return e;
        }
        return parse_test();
    }

    PmltqCondExpr parse_test() {
        if (cur_.type != PmltqTok::IDENT)
            throw_expected("attribute name");
        PmltqCondExpr t;
        t.kind = PmltqCondKind::Test;
        t.field = cur_.text;
        bump();
        t.op = parse_cmp_op();
        t.value = parse_value();
        return t;
    }

    std::string parse_cmp_op() {
        if (cur_.type == PmltqTok::EQ) {
            bump();
            return "=";
        }
        if (cur_.type == PmltqTok::NEQ) {
            bump();
            return "!=";
        }
        if (cur_.type == PmltqTok::EQEQ) {
            bump();
            return "=";
        }
        if (cur_.type == PmltqTok::TILDE) {
            bump();
            return "~";
        }
        if (cur_.type == PmltqTok::BANG) {
            bump();
            if (cur_.type != PmltqTok::TILDE)
                throw_expected("'~' after '!'");
            bump();
            return "!~";
        }
        throw_expected("comparison operator (=, !=, ==, ~, !~)");
    }

    std::string parse_value() {
        if (cur_.type == PmltqTok::STRING || cur_.type == PmltqTok::NUMBER) {
            std::string v = cur_.text;
            bump();
            return v;
        }
        if (cur_.type == PmltqTok::IDENT) {
            std::string v = cur_.text;
            bump();
            return v;
        }
        throw_expected("string, number, or identifier value");
    }

    PmltqOutputBlock parse_output_block() {
        PmltqOutputBlock ob;
        if (cur_.type == PmltqTok::IDENT && cur_.text == "distinct") {
            ob.saw_distinct_keyword = true;
            bump();
        }
        ob.items.push_back(parse_output_item());
        while (cur_.type == PmltqTok::COMMA) {
            bump();
            ob.items.push_back(parse_output_item());
        }
        return ob;
    }

    PmltqOutputItem parse_output_item() {
        if (cur_.type == PmltqTok::IDENT && cur_.text == "count") {
            bump();
            expect(PmltqTok::LPAREN);
            bump();
            if (cur_.type != PmltqTok::IDENT || cur_.text != "over")
                throw_expected("'over' inside count(...)");
            bump();
            std::pair<std::string, std::string> fr = parse_field_ref();
            expect(PmltqTok::RPAREN);
            bump();
            PmltqOutputItem it;
            it.kind = PmltqOutputItemKind::CountOver;
            it.node = std::move(fr.first);
            it.attr = std::move(fr.second);
            return it;
        }

        std::pair<std::string, std::string> fr = parse_field_ref();
        PmltqOutputItem it;
        it.kind = PmltqOutputItemKind::FieldRef;
        it.node = std::move(fr.first);
        it.attr = std::move(fr.second);
        return it;
    }

    std::pair<std::string, std::string> parse_field_ref() {
        if (cur_.type == PmltqTok::DOLLAR)
            bump();
        if (cur_.type != PmltqTok::IDENT)
            throw_expected("node name in field reference");
        std::string node = cur_.text;
        bump();
        if (cur_.type != PmltqTok::DOT)
            throw_expected("'.' in field reference");
        bump();
        if (cur_.type != PmltqTok::IDENT)
            throw_expected("attribute name in field reference");
        std::string attr = cur_.text;
        bump();
        return {std::move(node), std::move(attr)};
    }
};

} // namespace

PmltqQuery parse_pmltq_query(const std::string& input) {
    PmltqParser p(input);
    return p.parse_query();
}

} // namespace pando::pmltq
