#include "query/dialect/pmltq/pmltq_native_lower.h"

#include "query/dialect/pmltq/pmltq_test_lower.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pando::pmltq {

namespace {

ConditionPtr lower_cond_expr(const PmltqCondExpr& n, std::string& err) {
    switch (n.kind) {
    case PmltqCondKind::Test:
        return lower_pmltq_test_strings(n.field, n.op, n.value, err);
    case PmltqCondKind::And: {
        if (n.children.empty()) {
            err = "PMLTQ: empty 'and'";
            return nullptr;
        }
        ConditionPtr acc;
        for (const auto& c : n.children) {
            ConditionPtr p = lower_cond_expr(c, err);
            if (!p)
                return nullptr;
            if (!acc)
                acc = std::move(p);
            else
                acc = ConditionNode::make_branch(BoolOp::AND, acc, p);
        }
        return acc;
    }
    case PmltqCondKind::Or: {
        if (n.children.empty()) {
            err = "PMLTQ: empty 'or'";
            return nullptr;
        }
        ConditionPtr acc;
        for (const auto& c : n.children) {
            ConditionPtr p = lower_cond_expr(c, err);
            if (!p)
                return nullptr;
            if (!acc)
                acc = std::move(p);
            else
                acc = ConditionNode::make_branch(BoolOp::OR, acc, p);
        }
        return acc;
    }
    }
    err = "PMLTQ: internal condition kind";
    return nullptr;
}

std::vector<std::string> fields_from_output_block(const PmltqOutputBlock& ob) {
    std::vector<std::string> out;
    auto add = [&](const std::string& n, const std::string& a) {
        const std::string f = n + "." + a;
        if (std::find(out.begin(), out.end(), f) == out.end())
            out.push_back(f);
    };
    for (const auto& it : ob.items) {
        if (it.kind == PmltqOutputItemKind::CountOver)
            add(it.node, it.attr);
    }
    if (!out.empty())
        return out;
    for (const auto& it : ob.items) {
        if (it.kind == PmltqOutputItemKind::FieldRef)
            add(it.node, it.attr);
    }
    return out;
}

bool output_nodes_match_selector(const PmltqQuery& q, std::string& err) {
    if (q.selector.bind_name.empty())
        return true;
    for (const auto& it : q.output.items) {
        if (it.node != q.selector.bind_name) {
            err = "PMLTQ: field node '" + it.node + "' does not match selector binding $" +
                  q.selector.bind_name;
            return false;
        }
    }
    return true;
}

} // namespace

Program lower_native_pmltq_query(const PmltqQuery& q, std::string& err) {
    Program prog;
    ConditionPtr cond;
    if (q.selector.has_cond) {
        cond = lower_cond_expr(q.selector.cond, err);
        if (!cond) {
            if (err.empty())
                err = "PMLTQ: invalid condition";
            return prog;
        }
    }

    Statement stmt;
    stmt.has_query = true;
    QueryToken qt;
    qt.name = q.selector.bind_name;
    qt.conditions = std::move(cond);
    stmt.query.tokens.push_back(std::move(qt));
    prog.push_back(std::move(stmt));

    if (q.has_output) {
        if (!output_nodes_match_selector(q, err))
            return prog;
        std::vector<std::string> fields = fields_from_output_block(q.output);
        if (fields.empty()) {
            err = "PMLTQ: >> block needs field references or count(over node.attr)";
            return prog;
        }

        Statement cmd;
        cmd.has_command = true;
        cmd.command.type = CommandType::COUNT;
        cmd.command.fields = std::move(fields);
        prog.push_back(std::move(cmd));
    }

    return prog;
}

} // namespace pando::pmltq
