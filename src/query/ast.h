#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>

namespace manatree {

// ── Relation types between query tokens ─────────────────────────────────

enum class RelationType {
    SEQUENCE,        // [] []   — adjacent in text
    GOVERNS,         // [] > [] — head governs dependent
    GOVERNED_BY,     // [] < [] — dependent governed by head
    TRANS_GOVERNS,   // [] >> []
    TRANS_GOV_BY,    // [] << []
    NOT_GOVERNS,     // [] !> []
    NOT_GOV_BY,      // [] !< []
};

// ── Comparison operators ────────────────────────────────────────────────

enum class CompOp {
    EQ,       // =
    NEQ,      // !=
    LT,       // <
    GT,       // >
    LTE,      // <=
    GTE,      // >=
    REGEX,    // = /pattern/
};

// ── A single attribute condition inside [] ──────────────────────────────

struct AttrCondition {
    std::string attr;        // e.g. "upos", "lemma", "feats.Number"
    CompOp op = CompOp::EQ;
    std::string value;       // string/regex value
};

// ── Boolean combination of conditions ───────────────────────────────────

enum class BoolOp { AND, OR };

struct ConditionNode;
using ConditionPtr = std::shared_ptr<ConditionNode>;

struct ConditionNode {
    // Leaf: single attribute condition
    // Branch: boolean combination
    bool is_leaf = true;

    // Leaf fields
    AttrCondition leaf;

    // Branch fields
    BoolOp       bool_op = BoolOp::AND;
    ConditionPtr left;
    ConditionPtr right;

    static ConditionPtr make_leaf(AttrCondition c) {
        auto n = std::make_shared<ConditionNode>();
        n->is_leaf = true;
        n->leaf = std::move(c);
        return n;
    }
    static ConditionPtr make_branch(BoolOp op, ConditionPtr l, ConditionPtr r) {
        auto n = std::make_shared<ConditionNode>();
        n->is_leaf = false;
        n->bool_op = op;
        n->left = std::move(l);
        n->right = std::move(r);
        return n;
    }
};

// ── Repetition constants ────────────────────────────────────────────────

static constexpr int REPEAT_UNBOUNDED = 100;  // practical cap for {n,} and +

// ── A single query token ────────────────────────────────────────────────

struct QueryToken {
    std::string    name;              // optional label (e.g. "verb:")
    bool           is_target = false; // @[] marker
    ConditionPtr   conditions;        // may be null (empty token [])
    int            min_repeat = 1;    // {min,max} repetition; default 1,1 = exact single token
    int            max_repeat = 1;    // REPEAT_UNBOUNDED for {n,} and +

    bool has_repetition() const { return min_repeat != 1 || max_repeat != 1; }
};

// ── A relation edge between two tokens in the query chain ───────────────

struct QueryRelation {
    RelationType type;
};

// ── Global filter (#12): :: match.region_attr op value, or a.attr = b.attr ───

struct GlobalRegionFilter {
    std::string region_attr;   // e.g. "text_year" (region type + attr)
    CompOp      op = CompOp::EQ;
    std::string value;
};

struct GlobalAlignmentFilter {
    std::string name1, attr1;
    std::string name2, attr2;
};

// ── The full token query ────────────────────────────────────────────────

struct TokenQuery {
    std::vector<QueryToken>    tokens;
    std::vector<QueryRelation> relations;  // relations[i] is between tokens[i] and tokens[i+1]
    std::string                within;     // "s", "p", "text" or empty
    std::vector<GlobalRegionFilter>   global_region_filters;    // :: match.text_year > 2000
    std::vector<GlobalAlignmentFilter> global_alignment_filters; // :: a.tuid = b.tuid
};

// ── Display / grouping commands ─────────────────────────────────────────

enum class CommandType {
    CONCORDANCE,   // default: show matches
    COUNT,
    GROUP,
    SORT,
    FREQ,
    COLL,
    DCOLL,
    CAT,
    SIZE,
    RAW,
    SHOW_ATTRS,
    SHOW_REGIONS,
    SHOW_NAMED,
    TABULATE,
};

struct GroupCommand {
    CommandType type;
    std::string query_name;          // which named query to operate on
    std::vector<std::string> fields; // fields to group/sort by
    // coll/dcoll parameters
    int window = 5;
    int min_freq = 5;
    std::string deprel;
    std::vector<std::string> measures;
};

// ── Top-level statement ─────────────────────────────────────────────────

struct Statement {
    std::string name;                // if named query ("Nouns = ...")
    TokenQuery  query;               // the token query
    bool        has_query = false;

    // #16: Source | Target parallel query (when true, query is source, target_query is target)
    bool        is_parallel = false;
    TokenQuery  target_query;        // target side when is_parallel

    GroupCommand command;
    bool         has_command = false;
};

using Program = std::vector<Statement>;

} // namespace manatree
