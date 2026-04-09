#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pando::pmltq {

struct PmltqSpan {
    size_t begin = 0;
    size_t end = 0;
};

enum class PmltqSelectorKind {
    ANode,
    ARoot,
    Tok,
};

enum class PmltqCondKind {
    Test,
    And,
    Or,
};

// Condition tree: flat tests, and/or (no nested selectors).
struct PmltqCondExpr {
    PmltqCondKind kind = PmltqCondKind::Test;
    std::string field;
    std::string op;
    std::string value;
    std::vector<PmltqCondExpr> children;
};

struct PmltqSelector {
    PmltqSelectorKind kind = PmltqSelectorKind::ANode;
    std::string bind_name;
    bool has_cond = false;
    PmltqCondExpr cond;
};

// `>>` return block (subset): comma-separated `distinct` field refs and/or `count(over $node.attr)`.
enum class PmltqOutputItemKind {
    FieldRef,  // $a.form or a.form (from distinct list)
    CountOver, // count(over $a.form)
};

struct PmltqOutputItem {
    PmltqOutputItemKind kind = PmltqOutputItemKind::FieldRef;
    std::string node;
    std::string attr;
};

struct PmltqOutputBlock {
    bool saw_distinct_keyword = false;
    std::vector<PmltqOutputItem> items;
};

struct PmltqQuery {
    PmltqSelector selector;
    bool has_output = false;
    PmltqOutputBlock output;
};

} // namespace pando::pmltq
