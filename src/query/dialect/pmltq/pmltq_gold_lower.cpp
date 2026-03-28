#include "query/dialect/pmltq/pmltq_gold_lower.h"

#include "query/dialect/pmltq/pmltq_gold_json.h"
#include "query/dialect/pmltq/pmltq_test_lower.h"

#include <sstream>
#include <utility>

namespace manatree::pmltq {

namespace {

bool output_filters_nonempty(const JsonValue& query_ast) {
    const JsonValue* of = query_ast.get("outputFilters");
    if (!of || of->kind == JsonValue::Null)
        return false;
    if (of->kind == JsonValue::Object && of->obj_val.empty())
        return false;
    if (of->kind == JsonValue::Array && of->arr_val.empty())
        return false;
    return true;
}

ConditionPtr test_to_leaf(const JsonValue& t, std::string& err) {
    std::string field;
    std::string op;
    std::string val;

    if (const JsonValue* pr = t.get("predicate")) {
        const JsonValue* f = pr->get("field");
        const JsonValue* o = pr->get("operator");
        const JsonValue* v = pr->get("value");
        field = f ? f->as_string() : "";
        op = o ? o->as_string() : "";
        val = v ? v->as_string() : "";
    } else {
        const JsonValue* a = t.get("a");
        const JsonValue* o = t.get("operator");
        const JsonValue* b = t.get("b");
        field = a ? a->as_string() : "";
        op = o ? o->as_string() : "";
        if (b) {
            if (b->kind == JsonValue::String)
                val = b->s_val;
            else
                val = b->as_string();
        }
    }

    return lower_pmltq_test_strings(field, op, val, err);
}

ConditionPtr walk_conditions(const JsonValue& n, std::string& err) {
    const JsonValue* tp = n.get("type");
    if (!tp) {
        err = "gold AST: condition without type";
        return nullptr;
    }
    const std::string ty = tp->as_string();

    if (ty == "test")
        return test_to_leaf(n, err);

    if (ty == "and") {
        const JsonValue* ch = n.get("children");
        if (!ch || ch->kind != JsonValue::Array) {
            err = "gold AST: 'and' without children array";
            return nullptr;
        }
        ConditionPtr acc;
        for (const auto& c : ch->arr_val) {
            ConditionPtr p = walk_conditions(c, err);
            if (!p)
                return nullptr;
            if (!acc)
                acc = std::move(p);
            else
                acc = ConditionNode::make_branch(BoolOp::AND, acc, p);
        }
        return acc;
    }

    if (ty == "or") {
        const JsonValue* ch = n.get("children");
        if (!ch || ch->kind != JsonValue::Array) {
            err = "gold AST: 'or' without children array";
            return nullptr;
        }
        ConditionPtr acc;
        for (const auto& c : ch->arr_val) {
            ConditionPtr p = walk_conditions(c, err);
            if (!p)
                return nullptr;
            if (!acc)
                acc = std::move(p);
            else
                acc = ConditionNode::make_branch(BoolOp::OR, acc, p);
        }
        return acc;
    }

    if (ty == "selector" || ty == "relation" || ty == "subquery") {
        err = "PMLTQ: nested selector/relation/subquery not yet lowered to pando";
        return nullptr;
    }

    err = "PMLTQ: unsupported condition node type: " + ty;
    return nullptr;
}

ConditionPtr selector_conditions(const JsonValue& sel, std::string& err) {
    const JsonValue* raw = sel.get("children");
    if (!raw)
        return nullptr;

    if (raw->kind == JsonValue::Array) {
        if (raw->arr_val.empty())
            return nullptr;
        if (raw->arr_val.size() == 1)
            return walk_conditions(raw->arr_val[0], err);
        // Multiple top-level conditions — treat as implicit AND
        ConditionPtr acc;
        for (const auto& c : raw->arr_val) {
            ConditionPtr p = walk_conditions(c, err);
            if (!p)
                return nullptr;
            if (!acc)
                acc = std::move(p);
            else
                acc = ConditionNode::make_branch(BoolOp::AND, acc, p);
        }
        return acc;
    }

    return walk_conditions(*raw, err);
}

} // namespace

Program lower_gold_ast_to_program(const JsonValue& gold_response, std::string& err) {
    Program prog;
    const JsonValue* ok = gold_response.get("ok");
    if (ok && ok->kind == JsonValue::Bool && !ok->b_val) {
        const JsonValue* em = gold_response.get("error");
        err = em ? em->as_string() : "gold ok=false";
        return prog;
    }

    const JsonValue* ast = gold_response.get("ast");
    if (!ast || ast->kind != JsonValue::Object) {
        err = "gold response missing ast object";
        return prog;
    }

    const JsonValue* t = ast->get("type");
    if (!t || t->as_string() != "query") {
        err = "gold ast: expected root type \"query\"";
        return prog;
    }

    if (output_filters_nonempty(*ast)) {
        err = "PMLTQ: >> output filters not yet lowered to pando (gold has sql for these)";
        return prog;
    }

    const JsonValue* ch = ast->get("children");
    if (!ch || ch->kind != JsonValue::Array || ch->arr_val.empty()) {
        err = "gold ast: no selectors";
        return prog;
    }
    if (ch->arr_val.size() > 1) {
        err = "PMLTQ: multiple semicolon-separated selectors not yet lowered";
        return prog;
    }

    const JsonValue& sel = ch->arr_val[0];
    const JsonValue* st = sel.get("type");
    if (!st || st->as_string() != "selector") {
        err = "PMLTQ: expected selector (subquery not lowered yet)";
        return prog;
    }

    const JsonValue* nt = sel.get("nodeType");
    if (!nt) {
        err = "gold ast: selector without nodeType";
        return prog;
    }
    const std::string node_type = nt->as_string();
    if (node_type != "a-node" && node_type != "a-root" && node_type != "tok") {
        err = "PMLTQ: node type \"" + node_type + "\" not mapped yet";
        return prog;
    }

    std::string ec;
    ConditionPtr cond = selector_conditions(sel, ec);
    if (!ec.empty()) {
        err = std::move(ec);
        return prog;
    }

    Statement stmt;
    stmt.has_query = true;
    QueryToken qt;
    qt.conditions = std::move(cond);
    stmt.query.tokens.push_back(std::move(qt));

    prog.push_back(std::move(stmt));
    return prog;
}

} // namespace manatree::pmltq
