#include "query/executor.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <queue>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <functional>

#ifndef PANDO_USE_RE2
#include <mutex>
#endif

namespace manatree {

QueryExecutor::QueryExecutor(const Corpus& corpus) : corpus_(corpus) {}

const FoldMap& QueryExecutor::get_fold_map(const std::string& attr, bool case_fold, bool accent_fold) const {
    std::string key = attr + ":" + (case_fold ? "1" : "0") + (accent_fold ? "1" : "0");
    std::lock_guard<std::mutex> lock(fold_map_mutex_);
    auto it = fold_map_cache_.find(key);
    if (it != fold_map_cache_.end()) return it->second;
    const auto& lex = corpus_.attr(attr).lexicon();
    FoldMap fm;
    if (case_fold && accent_fold) fm = FoldMap::build_lc_no_accents(lex);
    else if (case_fold) fm = FoldMap::build_lowercase(lex);
    else fm = FoldMap::build_no_accents(lex);
    auto [ins, _] = fold_map_cache_.emplace(key, std::move(fm));
    return ins->second;
}

// ── Attribute name normalization ────────────────────────────────────────

std::string QueryExecutor::normalize_attr(const std::string& attr) const {
    if (attr.size() > 6 && attr.compare(0, 6, "feats.") == 0) {
        std::string split_name = "feats_" + attr.substr(6);
        if (corpus_.has_attr(split_name)) return split_name;
        return attr;
    }
    if (attr.size() > 6 && attr.compare(0, 6, "feats_") == 0) {
        if (corpus_.has_attr(attr)) return attr;
        return "feats." + attr.substr(6);
    }
    return attr;
}

// ── Combined feats helpers ──────────────────────────────────────────────
//
// When feats is stored as a single string like "Case=Nom|Number=Sing",
// extract the value for a specific feature name.

static bool is_feats_sub(const std::string& name, std::string& feat_name) {
    if (name.size() > 6 && name.compare(0, 6, "feats.") == 0) {
        feat_name = name.substr(6);
        return true;
    }
    return false;
}

static std::string extract_feat(std::string_view feats,
                                const std::string& feat_name) {
    if (feats == "_" || feats.empty()) return "_";

    std::string prefix = feat_name + "=";
    size_t search = 0;
    while (search < feats.size()) {
        size_t pos = feats.find(prefix, search);
        if (pos == std::string::npos) return "_";
        if (pos == 0 || feats[pos - 1] == '|') {
            size_t val_start = pos + prefix.size();
            size_t val_end = feats.find('|', val_start);
            if (val_end == std::string::npos) val_end = feats.size();
            return std::string(feats.substr(val_start, val_end - val_start));
        }
        search = pos + 1;
    }
    return "_";
}

static bool feats_entry_matches(std::string_view feats,
                                const std::string& feat_name,
                                const std::string& value) {
    return extract_feat(feats, feat_name) == value;
}

// ── Condition compilation (#25): pre-resolve EQ values to LexiconIds ────
//
// Walk the condition tree and resolve string EQ values to integer IDs.
// check_leaf can then compare id_at(pos) == resolved_id instead of
// value_at(pos) == string, avoiding a lexicon lookup per position.

void QueryExecutor::compile_conditions(const ConditionPtr& cond) const {
    if (!cond) return;
    if (cond->is_leaf) {
        AttrCondition& ac = const_cast<AttrCondition&>(cond->leaf);
        if (ac.op == CompOp::EQ && !ac.case_insensitive && !ac.diacritics_insensitive) {
            std::string name = normalize_attr(ac.attr);
            // Only resolve for positional attrs (not feats sub, not region attrs)
            std::string feat_name;
            if (!is_feats_sub(name, feat_name) && corpus_.has_attr(name)) {
                ac.resolved_id = corpus_.attr(name).lexicon().lookup(ac.value);
            }
        }
        return;
    }
    if (cond->is_structural) {
        compile_conditions(cond->nested_conditions);
        return;
    }
    compile_conditions(cond->left);
    compile_conditions(cond->right);
}

void QueryExecutor::compile_query(const TokenQuery& query) const {
    for (const auto& tok : query.tokens)
        compile_conditions(tok.conditions);
    compile_conditions(query.within_having);
    for (const auto& cc : query.containing_clauses)
        compile_conditions(cc.subtree_cond);
}

// ── Cardinality estimation ──────────────────────────────────────────────
//
// Uses the rev.idx to get exact counts in O(1) per EQ condition.
// For AND, takes min; for OR, takes sum.  This is a conservative upper
// bound that's free to compute.

size_t QueryExecutor::estimate_leaf(const AttrCondition& ac) const {
    std::string name = normalize_attr(ac.attr);

    // Combined feats mode: scan the feats lexicon for matching entries
    std::string feat_name;
    if (is_feats_sub(name, feat_name) && !corpus_.has_attr(name)
        && corpus_.has_attr("feats")) {
        const auto& pa = corpus_.attr("feats");
        size_t total = 0;
        LexiconId n = pa.lexicon().size();
        for (LexiconId id = 0; id < n; ++id) {
            if (feats_entry_matches(pa.lexicon().get(id), feat_name, ac.value))
                total += pa.count_of_id(id);
        }
        if (ac.op == CompOp::NEQ)
            return static_cast<size_t>(corpus_.size()) - total;
        return total;
    }

    if (!corpus_.has_attr(name)) {
        // Try region attribute fallback: name = "struct_region"
        auto us = name.find('_');
        if (us != std::string::npos && us + 1 < name.size()) {
            std::string struct_name = name.substr(0, us);
            std::string region_attr = name.substr(us + 1);
            if (corpus_.has_structure(struct_name)) {
                const auto& sa = corpus_.structure(struct_name);
                if (sa.has_region_attr(region_attr)) {
                    // Conservative estimate: we don't have a reverse index for region attrs,
                    // so return the corpus size as upper bound
                    return static_cast<size_t>(corpus_.size());
                }
            }
        }
        return 0;
    }
    const auto& pa = corpus_.attr(name);

    // Fold-aware cardinality estimation
    if ((ac.case_insensitive || ac.diacritics_insensitive) && ac.op == CompOp::EQ) {
        const auto& fm = get_fold_map(name, ac.case_insensitive, ac.diacritics_insensitive);
        std::string folded_query;
        if (ac.case_insensitive && ac.diacritics_insensitive)
            folded_query = FoldMap::to_lower_no_accents(ac.value);
        else if (ac.case_insensitive)
            folded_query = FoldMap::to_lower(ac.value);
        else
            folded_query = FoldMap::strip_accents(ac.value);
        const auto& ids = fm.lookup(folded_query);
        size_t total = 0;
        for (LexiconId id : ids)
            total += pa.count_of_id(id);
        return total;
    }

    switch (ac.op) {
        case CompOp::EQ:
            return pa.count_of(ac.value);
        case CompOp::NEQ: {
            size_t eq = pa.count_of(ac.value);
            return static_cast<size_t>(corpus_.size()) - eq;
        }
        default:
            return static_cast<size_t>(corpus_.size());
    }
}

size_t QueryExecutor::estimate_cardinality(const ConditionPtr& cond) const {
    if (!cond) return static_cast<size_t>(corpus_.size());
    if (cond->is_leaf) return estimate_leaf(cond->leaf);
    if (cond->is_structural) return static_cast<size_t>(corpus_.size());  // conservative estimate

    size_t l = estimate_cardinality(cond->left);
    size_t r = estimate_cardinality(cond->right);
    if (cond->bool_op == BoolOp::AND)
        return std::min(l, r);
    return std::min(l + r, static_cast<size_t>(corpus_.size()));
}

// ── Query planning ──────────────────────────────────────────────────────
//
// Picks the lowest-cardinality token as seed, then BFS outward along the
// query chain to determine step order.  This ensures we start from the
// most selective restriction and never materialize the large side.

QueryPlan QueryExecutor::plan_query(const TokenQuery& query) const {
    QueryPlan plan;
    size_t n = query.tokens.size();
    if (n == 0) return plan;

    std::vector<size_t> card(n);
    for (size_t i = 0; i < n; ++i)
        card[i] = estimate_cardinality(query.tokens[i].conditions);

    // Pick the lowest-cardinality *non-optional* token as seed.
    // Optional tokens (min_repeat == 0) can match nothing, so they make
    // terrible anchors — seed_len=0 would create a corrupt span (end < start).
    // If every token is optional, fall back to the lowest-cardinality one;
    // the seed_len guard below still protects us.
    plan.seed = 0;
    for (size_t i = 1; i < n; ++i) {
        bool cur_optional  = (query.tokens[plan.seed].min_repeat == 0);
        bool cand_optional = (query.tokens[i].min_repeat == 0);
        // Prefer non-optional over optional; among same optionality, prefer lower cardinality
        if ((!cand_optional && cur_optional) ||
            (cand_optional == cur_optional && card[i] < card[plan.seed]))
            plan.seed = i;
    }

    // BFS from seed along the linear chain
    std::vector<bool> visited(n, false);
    visited[plan.seed] = true;
    std::queue<size_t> q;
    q.push(plan.seed);

    while (!q.empty()) {
        size_t cur = q.front();
        q.pop();

        // Left neighbor: tokens[cur-1] connected by relations[cur-1]
        if (cur > 0 && !visited[cur - 1]) {
            visited[cur - 1] = true;
            plan.steps.push_back({cur, cur - 1, cur - 1, /*reversed=*/true});
            q.push(cur - 1);
        }
        // Right neighbor: tokens[cur+1] connected by relations[cur]
        if (cur + 1 < n && !visited[cur + 1]) {
            visited[cur + 1] = true;
            plan.steps.push_back({cur, cur + 1, cur, /*reversed=*/false});
            q.push(cur + 1);
        }
    }

    plan.cardinalities = std::move(card);
    return plan;
}

// ── Per-position condition checking ─────────────────────────────────────
//
// Evaluates a condition tree against a single corpus position.
// Avoids materializing candidate sets for non-seed tokens — just reads
// the per-position attribute value from the .dat file (O(1) array lookup).

bool QueryExecutor::check_leaf(CorpusPos pos, const AttrCondition& ac) const {
    std::string name = normalize_attr(ac.attr);

    // Combined feats mode: feats.Number="Sing" → check within combined feats string
    std::string feat_name;
    if (is_feats_sub(name, feat_name) && !corpus_.has_attr(name)
        && corpus_.has_attr("feats")) {
        const auto& pa = corpus_.attr("feats");
        std::string_view feats_str = pa.value_at(pos);
        std::string feat_val = extract_feat(feats_str, feat_name);
        switch (ac.op) {
            case CompOp::EQ:  return feat_val == ac.value;
            case CompOp::NEQ: return feat_val != ac.value;
            default:          return false;
        }
    }

    if (!corpus_.has_attr(name)) {
        // Try region attribute fallback: name = "struct_region"
        auto us = name.find('_');
        if (us != std::string::npos && us + 1 < name.size()) {
            std::string struct_name = name.substr(0, us);
            std::string region_attr = name.substr(us + 1);
            if (corpus_.has_structure(struct_name)) {
                const auto& sa = corpus_.structure(struct_name);
                if (sa.has_region_attr(region_attr)) {
                    int64_t rgn = sa.find_region(pos);
                    if (rgn >= 0) {
                        std::string_view val = sa.region_value(region_attr, static_cast<size_t>(rgn));
                        switch (ac.op) {
                            case CompOp::EQ:    return val == ac.value;
                            case CompOp::NEQ:   return val != ac.value;
                            case CompOp::REGEX: {
#ifdef PANDO_USE_RE2
                                const re2::RE2* compiled;
                                {
                                    std::lock_guard<std::mutex> lock(regex_cache_mutex_);
                                    auto it = regex_cache_.find(ac.value);
                                    if (it == regex_cache_.end())
                                        it = regex_cache_.emplace(ac.value, std::make_unique<re2::RE2>(ac.value)).first;
                                    compiled = it->second.get();
                                }
                                return re2::RE2::PartialMatch(val, *compiled);
#else
                                std::lock_guard<std::mutex> lock(regex_cache_mutex_);
                                auto it = regex_cache_.find(ac.value);
                                if (it == regex_cache_.end())
                                    it = regex_cache_.emplace(ac.value, std::regex(ac.value)).first;
                                std::string s(val);
                                return std::regex_search(s, it->second);
#endif
                            }
                            case CompOp::LT:    return val < ac.value;
                            case CompOp::GT:    return val > ac.value;
                            case CompOp::LTE:   return val <= ac.value;
                            case CompOp::GTE:   return val >= ac.value;
                        }
                    }
                }
            }
        }
        return false;
    }
    const auto& pa = corpus_.attr(name);

    // #25: Fast path — use pre-resolved LexiconId for integer comparison
    if (ac.resolved_id >= 0) {
        // resolved_id is only set for EQ without fold flags
        return pa.id_at(pos) == static_cast<LexiconId>(ac.resolved_id);
    }

    std::string_view val = pa.value_at(pos);

    // Fold-aware comparison for %c / %d flags
    if ((ac.case_insensitive || ac.diacritics_insensitive) &&
        (ac.op == CompOp::EQ || ac.op == CompOp::NEQ)) {
        std::string folded;
        if (ac.case_insensitive && ac.diacritics_insensitive)
            folded = FoldMap::to_lower_no_accents(val);
        else if (ac.case_insensitive)
            folded = FoldMap::to_lower(val);
        else
            folded = FoldMap::strip_accents(val);
        // The query value should already be folded by the caller (or we fold it here)
        std::string folded_query;
        if (ac.case_insensitive && ac.diacritics_insensitive)
            folded_query = FoldMap::to_lower_no_accents(ac.value);
        else if (ac.case_insensitive)
            folded_query = FoldMap::to_lower(ac.value);
        else
            folded_query = FoldMap::strip_accents(ac.value);
        if (ac.op == CompOp::EQ) return folded == folded_query;
        return folded != folded_query;
    }

    switch (ac.op) {
        case CompOp::EQ:    return val == ac.value;
        case CompOp::NEQ:   return val != ac.value;
        case CompOp::REGEX: {
#ifdef PANDO_USE_RE2
            // Look up or insert compiled RE2 under lock, then match outside lock.
            // RE2 objects are thread-safe for matching once constructed.
            const re2::RE2* compiled;
            {
                std::lock_guard<std::mutex> lock(regex_cache_mutex_);
                auto it = regex_cache_.find(ac.value);
                if (it == regex_cache_.end())
                    it = regex_cache_.emplace(ac.value, std::make_unique<re2::RE2>(ac.value)).first;
                compiled = it->second.get();
            }
            return re2::RE2::PartialMatch(val, *compiled);
#else
            std::lock_guard<std::mutex> lock(regex_cache_mutex_);
            auto it = regex_cache_.find(ac.value);
            if (it == regex_cache_.end())
                it = regex_cache_.emplace(ac.value, std::regex(ac.value)).first;
            std::string s(val);
            return std::regex_search(s, it->second);
#endif
        }
        case CompOp::LT:    return val < ac.value;
        case CompOp::GT:    return val > ac.value;
        case CompOp::LTE:   return val <= ac.value;
        case CompOp::GTE:   return val >= ac.value;
    }
    return false;
}

bool QueryExecutor::check_conditions(CorpusPos pos,
                                     const ConditionPtr& cond) const {
    if (!cond) return true;
    if (cond->is_leaf) return check_leaf(pos, cond->leaf);

    if (cond->is_structural) {
        if (!corpus_.has_deps())
            throw std::runtime_error("Structural conditions require dependency index");
        const auto& deps = corpus_.deps();
        std::vector<CorpusPos> related;
        switch (cond->struct_rel) {
            case StructRelType::CHILD:      related = deps.children(pos); break;
            case StructRelType::PARENT:     { auto h = deps.head(pos); if (h != NO_HEAD) related.push_back(h); break; }
            case StructRelType::SIBLING:    { auto h = deps.head(pos); if (h != NO_HEAD) { related = deps.children(h); related.erase(std::remove(related.begin(), related.end(), pos), related.end()); } break; }
            case StructRelType::DESCENDANT: related = deps.subtree(pos); break;
            case StructRelType::ANCESTOR:   related = deps.ancestors(pos); break;
        }
        bool any_match = false;
        for (CorpusPos rp : related) {
            if (check_conditions(rp, cond->nested_conditions)) {
                any_match = true;
                break;
            }
        }
        return cond->struct_negated ? !any_match : any_match;
    }

    if (cond->bool_op == BoolOp::AND)
        return check_conditions(pos, cond->left) &&
               check_conditions(pos, cond->right);
    else
        return check_conditions(pos, cond->left) ||
               check_conditions(pos, cond->right);
}

// ── Relation traversal ──────────────────────────────────────────────────
//
// Given a position on one side of a relation edge, returns the positions
// on the other side.  For SEQUENCE this is 1 position; for GOVERNS it's
// the children list or the single head; for transitive relations it walks
// the tree (walk-up is O(depth), walk-down is DFS).

static bool is_dep_relation(RelationType rel) {
    return rel != RelationType::SEQUENCE;
}

std::vector<CorpusPos> QueryExecutor::find_related(
        CorpusPos pos, RelationType rel, bool reversed) const {

    std::vector<CorpusPos> out;

    // Defensive: ignore invalid positions to avoid out-of-bounds in deps/attrs
    if (pos < 0 || pos >= corpus_.size())
        return out;

    if (is_dep_relation(rel) && !corpus_.has_deps())
        throw std::runtime_error(
            "Query uses dependency relations but corpus has no dependency index");

    const auto& deps = corpus_.deps();

    // Invert the relation when traversing backward through the edge
    RelationType eff = rel;
    if (reversed) {
        switch (rel) {
            case RelationType::SEQUENCE:       eff = RelationType::SEQUENCE; break;
            case RelationType::GOVERNS:        eff = RelationType::GOVERNED_BY; break;
            case RelationType::GOVERNED_BY:    eff = RelationType::GOVERNS; break;
            case RelationType::TRANS_GOVERNS:  eff = RelationType::TRANS_GOV_BY; break;
            case RelationType::TRANS_GOV_BY:   eff = RelationType::TRANS_GOVERNS; break;
            case RelationType::NOT_GOVERNS:    eff = RelationType::NOT_GOV_BY; break;
            case RelationType::NOT_GOV_BY:     eff = RelationType::NOT_GOVERNS; break;
        }
    }

    switch (eff) {
        case RelationType::SEQUENCE:
            if (reversed) {
                if (pos > 0) out.push_back(pos - 1);
            } else {
                if (pos + 1 < corpus_.size()) out.push_back(pos + 1);
            }
            break;

        case RelationType::GOVERNS:
            out = deps.children(pos);
            break;

        case RelationType::GOVERNED_BY: {
            CorpusPos h = deps.head(pos);
            if (h != NO_HEAD) out.push_back(h);
            break;
        }

        case RelationType::TRANS_GOVERNS:
            out = deps.subtree(pos);
            break;

        case RelationType::TRANS_GOV_BY:
            out = deps.ancestors(pos);
            break;

        case RelationType::NOT_GOVERNS:
        case RelationType::NOT_GOV_BY:
            // Negative relations handled as post-filters in execute()
            break;
    }
    return out;
}

// ── Lazy seed iteration (avoids materializing full position vector for EQ) ─

void QueryExecutor::for_each_seed_position(const ConditionPtr& cond,
                                           std::function<bool(CorpusPos)> f) const {
    if (!cond) {
        for (CorpusPos p = 0; p < corpus_.size(); ++p)
            if (!f(p)) return;
        return;
    }
    if (cond->is_leaf) {
        const AttrCondition& ac = cond->leaf;
        std::string name = normalize_attr(ac.attr);
        std::string feat_name;
        if (is_feats_sub(name, feat_name) && !corpus_.has_attr(name)
            && corpus_.has_attr("feats")) {
            auto vec = resolve_leaf(ac);
            for (CorpusPos p : vec)
                if (!f(p)) return;
            return;
        }
        if (!corpus_.has_attr(name)) {
            auto us = name.find('_');
            if (us != std::string::npos && us + 1 < name.size()) {
                std::string struct_name = name.substr(0, us);
                std::string region_attr = name.substr(us + 1);
                if (corpus_.has_structure(struct_name)) {
                    const auto& sa = corpus_.structure(struct_name);
                    if (sa.has_region_attr(region_attr)) {
                        auto vec = resolve_leaf(ac);
                        for (CorpusPos p : vec)
                            if (!f(p)) return;
                        return;
                    }
                }
            }
            return;
        }
        const auto& pa = corpus_.attr(name);
        if (ac.op == CompOp::EQ && !ac.case_insensitive && !ac.diacritics_insensitive) {
            // #25: Use pre-resolved ID if available, otherwise lookup
            LexiconId id = (ac.resolved_id >= 0)
                ? static_cast<LexiconId>(ac.resolved_id)
                : pa.lexicon().lookup(ac.value);
            if (id == UNKNOWN_LEX) return;
            pa.for_each_position_id(id, f);
            return;
        }
        // NEQ, REGEX, or fold-aware EQ: materialize
        auto vec = resolve_leaf(ac);
        for (CorpusPos p : vec)
            if (!f(p)) return;
        return;
    }
    if (cond->bool_op == BoolOp::AND) {
        size_t left_est  = estimate_cardinality(cond->left);
        size_t right_est = estimate_cardinality(cond->right);
        const ConditionPtr& cheap     = (left_est <= right_est) ? cond->left : cond->right;
        const ConditionPtr& expensive = (left_est <= right_est) ? cond->right : cond->left;
        for_each_seed_position(cheap, [&](CorpusPos p) {
            return check_conditions(p, expensive) ? f(p) : true;
        });
        return;
    }
    // OR: need merged list
    auto left  = resolve_conditions(cond->left);
    auto right = resolve_conditions(cond->right);
    auto merged = unite(left, right);
    for (CorpusPos p : merged)
        if (!f(p)) return;
}

// ── Seed resolution (inverted index lookup) ─────────────────────────────

std::vector<CorpusPos> QueryExecutor::resolve_leaf(
        const AttrCondition& ac) const {
    std::string name = normalize_attr(ac.attr);

    // Combined feats mode: scan feats lexicon, union matching position lists
    std::string feat_name;
    if (is_feats_sub(name, feat_name) && !corpus_.has_attr(name)
        && corpus_.has_attr("feats")) {
        const auto& pa = corpus_.attr("feats");
        LexiconId n = pa.lexicon().size();

        std::vector<CorpusPos> result;
        for (LexiconId id = 0; id < n; ++id) {
            bool match = feats_entry_matches(pa.lexicon().get(id),
                                             feat_name, ac.value);
            if ((ac.op == CompOp::EQ && match) ||
                (ac.op == CompOp::NEQ && !match)) {
                auto positions = pa.positions_of_id(id);
                result.insert(result.end(), positions.begin(), positions.end());
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    if (!corpus_.has_attr(name)) {
        // Try region attribute fallback: name = "struct_region"
        auto us = name.find('_');
        if (us != std::string::npos && us + 1 < name.size()) {
            std::string struct_name = name.substr(0, us);
            std::string region_attr = name.substr(us + 1);
            if (corpus_.has_structure(struct_name)) {
                const auto& sa = corpus_.structure(struct_name);
                if (sa.has_region_attr(region_attr)) {
                    // For region attributes, we support EQ by linear scan
                    if (ac.op == CompOp::EQ) {
                        std::vector<CorpusPos> result;
                        for (CorpusPos pos = 0; pos < corpus_.size(); ++pos) {
                            int64_t rgn = sa.find_region(pos);
                            if (rgn >= 0) {
                                std::string_view val = sa.region_value(region_attr, static_cast<size_t>(rgn));
                                if (val == ac.value) {
                                    result.push_back(pos);
                                }
                            }
                        }
                        return result;
                    }
                    // For other operations on region attrs, return empty (not supported)
                    return {};
                }
            }
        }
        return {};
    }
    const auto& pa = corpus_.attr(name);

    // Fold-aware EQ resolution via fold map
    if ((ac.case_insensitive || ac.diacritics_insensitive) && ac.op == CompOp::EQ) {
        const auto& fm = get_fold_map(name, ac.case_insensitive, ac.diacritics_insensitive);
        std::string folded_query;
        if (ac.case_insensitive && ac.diacritics_insensitive)
            folded_query = FoldMap::to_lower_no_accents(ac.value);
        else if (ac.case_insensitive)
            folded_query = FoldMap::to_lower(ac.value);
        else
            folded_query = FoldMap::strip_accents(ac.value);
        const auto& ids = fm.lookup(folded_query);
        if (ids.empty()) return {};
        if (ids.size() == 1) return pa.positions_of_id(ids[0]);
        // Union posting lists of all matching lex IDs
        std::vector<CorpusPos> result;
        for (LexiconId id : ids) {
            auto pos = pa.positions_of_id(id);
            result.insert(result.end(), pos.begin(), pos.end());
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    switch (ac.op) {
        case CompOp::EQ:
            return pa.positions_of(ac.value);
        case CompOp::NEQ:
            return pa.positions_not(ac.value, corpus_.size());
        case CompOp::REGEX: {
#ifdef PANDO_USE_RE2
            // Compile RE2 under lock, then match outside lock (thread-safe).
            const re2::RE2* compiled;
            {
                std::lock_guard<std::mutex> lock(regex_cache_mutex_);
                auto it = regex_cache_.find(ac.value);
                if (it == regex_cache_.end())
                    it = regex_cache_.emplace(ac.value, std::make_unique<re2::RE2>(ac.value)).first;
                compiled = it->second.get();
            }
            return pa.positions_matching(*compiled);
#else
            std::lock_guard<std::mutex> lock(regex_cache_mutex_);
            auto it = regex_cache_.find(ac.value);
            if (it == regex_cache_.end())
                it = regex_cache_.emplace(ac.value, std::regex(ac.value)).first;
            return pa.positions_matching(it->second);
#endif
        }
        default:
            throw std::runtime_error("Unsupported comparison on positional attr");
    }
}

std::vector<CorpusPos> QueryExecutor::resolve_conditions(
        const ConditionPtr& cond) const {
    if (!cond) {
        std::vector<CorpusPos> all(static_cast<size_t>(corpus_.size()));
        for (CorpusPos i = 0; i < corpus_.size(); ++i)
            all[static_cast<size_t>(i)] = i;
        return all;
    }
    if (cond->is_leaf) return resolve_leaf(cond->leaf);

    if (cond->bool_op == BoolOp::AND) {
        // Resolve the cheaper side, filter the results by the expensive side.
        // This avoids materializing huge complement sets for NEQ conditions
        // (e.g., [upos!="PUNCT" & lemma="cat"] only materializes "cat" positions).
        size_t left_est  = estimate_cardinality(cond->left);
        size_t right_est = estimate_cardinality(cond->right);

        const ConditionPtr& cheap     = (left_est <= right_est) ? cond->left : cond->right;
        const ConditionPtr& expensive = (left_est <= right_est) ? cond->right : cond->left;

        auto positions = resolve_conditions(cheap);
        positions.erase(
            std::remove_if(positions.begin(), positions.end(),
                [&](CorpusPos p) { return !check_conditions(p, expensive); }),
            positions.end());
        return positions;
    } else {
        auto left  = resolve_conditions(cond->left);
        auto right = resolve_conditions(cond->right);
        return unite(left, right);
    }
}

// ── Value comparison helper (used by both inline and post-hoc filters) ──

static bool compare_value(CompOp op, const std::string& a, const std::string& b) {
    switch (op) {
        case CompOp::EQ:  return a == b;
        case CompOp::NEQ: return a != b;
        case CompOp::LT:  return a < b;
        case CompOp::GT:  return a > b;
        case CompOp::LTE: return a <= b;
        case CompOp::GTE: return a >= b;
        default: return a == b;
    }
}

// ── Main execution ──────────────────────────────────────────────────────

namespace {

bool build_aggregate_plan(const Corpus& corpus, const std::vector<std::string>& fields,
                          AggregateBucketData& out) {
    out.columns.clear();
    out.region_intern.clear();
    out.counts.clear();
    out.total_hits = 0;
    out.columns.reserve(fields.size());
    out.region_intern.resize(fields.size());
    for (const std::string& field : fields) {
        AggregateBucketData::Column col;
        std::string attr_spec = field;
        if (field.rfind("match.", 0) == 0 && field.size() > 6) {
            attr_spec = field.substr(6);
        } else {
            auto dot = field.find('.');
            if (dot != std::string::npos && dot > 0) {
                col.named_anchor = field.substr(0, dot);
                attr_spec = field.substr(dot + 1);
            }
        }
        std::string attr = attr_spec;
        if (attr.size() > 5 && attr.substr(0, 5) == "feats" && attr.find('.') != std::string::npos)
            attr[attr.find('.')] = '_';
        if (corpus.has_attr(attr)) {
            col.kind = AggregateBucketData::Column::Kind::Positional;
            col.pa = &corpus.attr(attr);
            out.columns.push_back(std::move(col));
            continue;
        }
        bool found_reg = false;
        for (const auto& ra_name : corpus.region_attr_names()) {
            if (ra_name != attr_spec) continue;
            auto us = ra_name.find('_');
            if (us == std::string::npos || us + 1 >= ra_name.size()) return false;
            std::string sn = ra_name.substr(0, us);
            std::string ran = ra_name.substr(us + 1);
            if (!corpus.has_structure(sn)) return false;
            const auto& sa = corpus.structure(sn);
            if (!sa.has_region_attr(ran)) return false;
            col.kind = AggregateBucketData::Column::Kind::Region;
            col.sa = &sa;
            col.region_attr_name = std::move(ran);
            out.columns.push_back(std::move(col));
            found_reg = true;
            break;
        }
        if (!found_reg) return false;
    }
    return true;
}

bool fill_aggregate_key(AggregateBucketData& data, const Match& m, const NameIndexMap& nm,
                        std::vector<int64_t>& key_out) {
    key_out.resize(data.columns.size());
    for (size_t i = 0; i < data.columns.size(); ++i) {
        const auto& col = data.columns[i];
        CorpusPos pos = col.named_anchor.empty() ? m.first_pos()
                                                 : resolve_name(m, nm, col.named_anchor);
        if (pos == NO_HEAD) return false;
        if (col.kind == AggregateBucketData::Column::Kind::Positional) {
            key_out[i] = static_cast<int64_t>(col.pa->id_at(pos));
        } else {
            int64_t rgn = col.sa->find_region(pos);
            if (rgn < 0) return false;
            std::string val(col.sa->region_value(col.region_attr_name, static_cast<size_t>(rgn)));
            auto& st = data.region_intern[i];
            auto it = st.str_to_id.find(val);
            if (it != st.str_to_id.end()) {
                key_out[i] = it->second;
            } else {
                int64_t id = static_cast<int64_t>(st.id_to_str.size() + 1);
                st.str_to_id.emplace(val, id);
                st.id_to_str.push_back(std::move(val));
                key_out[i] = id;
            }
        }
    }
    return true;
}

} // namespace

size_t AggregateBucketData::VecHash::operator()(const std::vector<int64_t>& v) const noexcept {
    size_t h = v.size();
    for (int64_t x : v) {
        uint64_t ux = static_cast<uint64_t>(x);
        h ^= std::hash<uint64_t>{}(ux + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    }
    return h;
}

std::string decode_aggregate_bucket_key(const AggregateBucketData& data,
                                          const std::vector<int64_t>& key) {
    std::string out;
    for (size_t i = 0; i < key.size() && i < data.columns.size(); ++i) {
        if (i > 0) out += '\t';
        const auto& col = data.columns[i];
        if (col.kind == AggregateBucketData::Column::Kind::Positional) {
            LexiconId lid = static_cast<LexiconId>(key[i]);
            out += col.pa->lexicon().get(lid);
        } else {
            int64_t id = key[i];
            const auto& st = data.region_intern[i];
            if (id >= 1 && static_cast<size_t>(id) <= st.id_to_str.size())
                out += st.id_to_str[static_cast<size_t>(id - 1)];
        }
    }
    return out;
}

bool QueryExecutor::match_survives_post_filters_for_aggregate(
        const TokenQuery& query,
        const NameIndexMap& name_map,
        const std::vector<AnchorConstraint>& anchor_constraints,
        MatchSet& scratch,
        const Match& m) const {
    scratch.matches.clear();
    scratch.matches.push_back(m);
    scratch.total_count = 1;
    apply_anchor_filters(anchor_constraints, scratch);
    if (scratch.matches.empty()) return false;
    apply_within_having(query, scratch);
    if (scratch.matches.empty()) return false;
    apply_not_within(query, scratch);
    if (scratch.matches.empty()) return false;
    apply_containing(query, scratch);
    if (scratch.matches.empty()) return false;
    apply_position_orders(query, name_map, scratch);
    if (scratch.matches.empty()) return false;
    apply_global_filters(query, name_map, scratch);
    return !scratch.matches.empty();
}

MatchSet QueryExecutor::execute(const TokenQuery& query,
                                size_t max_matches,
                                bool count_total,
                                size_t max_total_cap,
                                size_t sample_size,
                                uint32_t random_seed,
                                unsigned num_threads,
                                const std::vector<std::string>* aggregate_by_fields) {
    // Strip region anchors (<s>, </s>) from query, recording constraints
    std::vector<AnchorConstraint> anchor_constraints;
    bool has_anchors = false;
    for (const auto& tok : query.tokens) {
        if (tok.is_anchor()) { has_anchors = true; break; }
    }
    TokenQuery stripped_query;
    if (has_anchors) stripped_query = strip_anchors(query, anchor_constraints);
    const TokenQuery& q = has_anchors ? stripped_query : query;

    MatchSet result;
    if (q.tokens.empty()) return result;
    size_t n = q.tokens.size();
    result.num_tokens = n;
    NameIndexMap name_map = build_name_map(q);

    // #25: Pre-resolve EQ values to LexiconIds for fast integer comparison
    compile_query(q);

    std::shared_ptr<AggregateBucketData> agg_storage;
    AggregateBucketData* agg_ptr = nullptr;
    MatchSet post_scratch;
    bool agg_capped = false;
    if (aggregate_by_fields && !aggregate_by_fields->empty() && sample_size == 0) {
        agg_storage = std::make_shared<AggregateBucketData>();
        if (build_aggregate_plan(corpus_, *aggregate_by_fields, *agg_storage))
            agg_ptr = agg_storage.get();
        else
            agg_storage.reset();
    }
    if (agg_ptr)
        post_scratch.matches.reserve(1);

    // Per-match post-filter pass is expensive at millions of hits; skip when only
    // inline :: region filters apply (handled above) and no other post-filters exist.
    const bool agg_per_match_post =
        agg_ptr
        && (!anchor_constraints.empty()
            || (q.within_having && !q.within.empty() && corpus_.has_structure(q.within))
            || (q.not_within && !q.within.empty() && corpus_.has_structure(q.within))
            || !q.containing_clauses.empty() || !q.position_orders.empty()
            || !q.global_alignment_filters.empty());

    unsigned eff_threads = (agg_ptr && num_threads > 1) ? 1u : num_threads;

    std::vector<Match> reservoir;
    if (sample_size > 0) reservoir.reserve(sample_size);
    std::mt19937 rng(random_seed != 0 ? random_seed : static_cast<uint32_t>(std::random_device{}()));

    // Partial match vectors are of size 2*n: [0..n-1]=starts, [n..2n-1]=ends
    // For non-repeating tokens: pm[i] == pm[n+i]
    auto build_match = [n](std::vector<CorpusPos>&& pm) -> Match {
        Match m;
        m.positions.assign(pm.begin(), pm.begin() + n);
        m.span_ends.assign(pm.begin() + n, pm.begin() + 2 * n);
        return m;
    };

    // Inline region-filter check so matches that fail :: filters are never
    // counted against the limit.  Runs in O(filters) per candidate — typically 1.
    auto pass_region_filters = [&](const Match& m) -> bool {
        for (const auto& gf : q.global_region_filters) {
            size_t us = gf.region_attr.find('_');
            if (us == std::string::npos || us + 1 >= gf.region_attr.size()) return false;
            std::string struct_name = gf.region_attr.substr(0, us);
            std::string attr_name = gf.region_attr.substr(us + 1);
            if (!corpus_.has_structure(struct_name)) return false;
            const auto& sa = corpus_.structure(struct_name);
            if (!sa.has_region_attr(attr_name)) return false;
            CorpusPos pos = m.first_pos();
            if (!gf.anchor_name.empty()) {
                CorpusPos ap = resolve_name(m, name_map, gf.anchor_name);
                if (ap != NO_HEAD) pos = ap;
            }
            int64_t rgn = sa.find_region(pos);
            if (rgn < 0) return false;
            std::string rval(sa.region_value(attr_name, static_cast<size_t>(rgn)));
            if (!compare_value(gf.op, rval, gf.value)) return false;
        }
        return true;
    };

    auto add_match = [&](std::vector<CorpusPos>&& positions) {
        Match m = build_match(std::move(positions));
        // Apply :: region filters inline so rejected matches don't consume the limit
        if (!q.global_region_filters.empty() && !pass_region_filters(m))
            return;
        if (agg_ptr) {
            if (agg_per_match_post &&
                !match_survives_post_filters_for_aggregate(q, name_map, anchor_constraints,
                                                          post_scratch, m))
                return;
            std::vector<int64_t> akey;
            if (!fill_aggregate_key(*agg_ptr, m, name_map, akey))
                return;
            if (max_total_cap > 0 && agg_ptr->total_hits >= max_total_cap) {
                agg_capped = true;
                return;
            }
            ++agg_ptr->total_hits;
            ++agg_ptr->counts[std::move(akey)];
            return;
        }
        ++result.total_count;
        if (sample_size > 0) {
            if (reservoir.size() < sample_size) {
                reservoir.push_back(std::move(m));
            } else {
                std::uniform_real_distribution<double> u(0, 1);
                if (u(rng) < static_cast<double>(sample_size) / static_cast<double>(result.total_count)) {
                    std::uniform_int_distribution<size_t> idx(0, sample_size - 1);
                    reservoir[idx(rng)] = std::move(m);
                }
            }
        } else if (max_matches == 0 || result.matches.size() < max_matches) {
            result.matches.push_back(std::move(m));
        }
    };

    auto reached_limit = [&]() {
        if (sample_size > 0) return false;  // sampling needs full enumeration
        if (agg_ptr) return false;
        return max_matches > 0 && result.total_count >= max_matches
               && !count_total;
    };
    auto reached_total_cap = [&]() {
        if (agg_ptr) return agg_capped;
        return count_total && max_total_cap > 0 && result.total_count >= max_total_cap;
    };

    // ── Single-token fast path ──────────────────────────────────────────

    if (n == 1) {
        size_t est = estimate_cardinality(q.tokens[0].conditions);
        result.cardinalities = {est};
        result.seed_token = 0;

        int min_rep = q.tokens[0].min_repeat;
        int max_rep = q.tokens[0].max_repeat;

        auto try_spans_from = [&](CorpusPos p) {
            // For single-token query with repetition: try spans starting at p
            for (int len = min_rep; len <= max_rep; ++len) {
                CorpusPos end = p + len - 1;
                if (end >= corpus_.size()) break;
                if (len > 1 && !check_conditions(end, q.tokens[0].conditions)) break;
                // All positions p..end must match (position p already verified by caller)
                // Positions p+1..end-1 checked incrementally below
                bool valid = true;
                if (len == 1) {
                    // already checked by caller
                } else {
                    // We verified p (caller) and end (above); check intermediate
                    for (CorpusPos cp = p + 1; cp < end; ++cp) {
                        if (!check_conditions(cp, q.tokens[0].conditions)) {
                            valid = false;
                            break;
                        }
                    }
                }
                if (!valid) break;
                std::vector<CorpusPos> pm = {p, end};  // 2*1: start, end
                add_match(std::move(pm));
                if (reached_limit() || reached_total_cap()) return false;
            }
            return !reached_limit() && !reached_total_cap();
        };

        bool use_scan = !q.tokens[0].conditions
                        || est > static_cast<size_t>(corpus_.size()) / 2;

        if (use_scan) {
            for (CorpusPos p = 0; p < corpus_.size(); ++p) {
                if (check_conditions(p, q.tokens[0].conditions)) {
                    if (!try_spans_from(p)) break;
                }
            }
        } else {
            for_each_seed_position(q.tokens[0].conditions, [&](CorpusPos p) {
                return try_spans_from(p);
            });
        }
        if (agg_ptr) {
            result.matches.clear();
            result.total_count = agg_ptr->total_hits;
            result.total_exact = !agg_capped;
            result.aggregate_buckets = std::move(agg_storage);
            return result;
        }
        apply_anchor_filters(anchor_constraints, result);
        apply_within_having(q, result);
        apply_not_within(q, result);
        apply_containing(q, result);
        apply_position_orders(q, name_map, result);
        apply_global_filters(q, name_map, result);
        result.total_exact = !reached_limit() && !reached_total_cap();
        return result;
    }

    // ── Plan: pick seed by cardinality, expand outward ──────────────────

    QueryPlan plan = plan_query(q);
    result.seed_token = plan.seed;
    result.cardinalities = plan.cardinalities;

    // #9: Use default_within from corpus when query does not specify within
    std::string effective_within = q.within.empty()
        ? corpus_.default_within() : q.within;
    bool has_within = !effective_within.empty() &&
                      corpus_.has_structure(effective_within);
    const StructuralAttr* within_sa = has_within
        ? &corpus_.structure(effective_within) : nullptr;

    // Parallel path: materialize seeds and process chunks in parallel (multi-token only)
    if (eff_threads > 1 && n > 1) {
        std::vector<CorpusPos> seeds = resolve_conditions(q.tokens[plan.seed].conditions);
        // #24: Pre-filter seeds that fall outside any within-region (parallel path).
        // Seeds are sorted, so cursor-based scan is efficient.
        if (within_sa && !seeds.empty()) {
            int64_t hint = -1;
            seeds.erase(std::remove_if(seeds.begin(), seeds.end(), [&](CorpusPos p) {
                int64_t rgn = (hint >= 0)
                    ? within_sa->find_region_from(p, hint)
                    : within_sa->find_region(p);
                if (rgn < 0) return true;  // remove: not in any region
                hint = rgn;
                return false;
            }), seeds.end());
        }
        if (!seeds.empty()) {
            size_t nw = std::min(static_cast<size_t>(eff_threads), seeds.size());
            size_t chunk_sz = (seeds.size() + nw - 1) / nw;
            std::vector<std::vector<Match>> thread_matches(nw);
            std::vector<std::thread> workers;
            for (size_t w = 0; w < nw; ++w) {
                size_t start = w * chunk_sz;
                size_t end = std::min(start + chunk_sz, seeds.size());
                workers.emplace_back([this, &q, &plan, within_sa, &seeds,
                                      &thread_matches, start, end, w]() {
                    for (size_t i = start; i < end; ++i) {
                        auto matches = expand_one_seed(q, plan, within_sa, seeds[i]);
                        for (auto& m : matches)
                            thread_matches[w].push_back(std::move(m));
                    }
                });
            }
            for (auto& t : workers) t.join();
            result.total_count = 0;
            for (const auto& vec : thread_matches) result.total_count += vec.size();
            for (auto& vec : thread_matches)
                for (auto& m : vec)
                    result.matches.push_back(std::move(m));
            if (sample_size > 0 && result.matches.size() > sample_size) {
                std::mt19937 rng(random_seed != 0 ? random_seed : static_cast<uint32_t>(std::random_device{}()));
                std::shuffle(result.matches.begin(), result.matches.end(), rng);
                result.matches.resize(sample_size);
            } else if (max_matches > 0 && !count_total && result.matches.size() > max_matches) {
                result.matches.resize(max_matches);
            }
            apply_anchor_filters(anchor_constraints, result);
            apply_within_having(q, result);
            apply_not_within(q, result);
            apply_containing(q, result);
            apply_position_orders(q, name_map, result);
            apply_global_filters(q, name_map, result);
            result.total_exact = true;
            return result;
        }
    }

    // Sequential path: process one seed at a time (lazy when possible)
    // #28: Maintain within-region cursor across seeds for O(1) amortized lookup
    // #24: Pre-check within-region for seed position before expand_seed.
    //      Seeds that fall outside any within-region are skipped entirely,
    //      avoiding the full expand_seed call.
    int64_t within_hint = -1;
    for_each_seed_position(q.tokens[plan.seed].conditions, [&](CorpusPos seed_p) {
        // #24: Early within-region rejection at seed level
        if (within_sa) {
            int64_t rgn = (within_hint >= 0)
                ? within_sa->find_region_from(seed_p, within_hint)
                : within_sa->find_region(seed_p);
            if (rgn < 0) return true;  // seed not in any region — skip, continue
            within_hint = rgn;         // advance cursor for next seed
        }
        expand_seed(q, plan, within_sa, seed_p, [&](std::vector<CorpusPos>&& pm) -> bool {
            add_match(std::move(pm));
            return !reached_limit() && !reached_total_cap();
        }, within_sa ? &within_hint : nullptr);
        return !reached_limit() && !reached_total_cap();
    });

    if (sample_size > 0 && !reservoir.empty())
        result.matches = std::move(reservoir);
    if (agg_ptr) {
        result.matches.clear();
        result.total_count = agg_ptr->total_hits;
        result.total_exact = !agg_capped;
        result.aggregate_buckets = std::move(agg_storage);
        return result;
    }
    apply_anchor_filters(anchor_constraints, result);
    apply_within_having(q, result);
    apply_not_within(q, result);
    apply_containing(q, result);
    apply_position_orders(q, name_map, result);
    apply_global_filters(q, name_map, result);
    result.total_exact = !reached_limit() && !reached_total_cap();
    return result;
}

// ── Shared seed expansion (single source of truth for match logic) ───────

void QueryExecutor::expand_seed(const TokenQuery& query,
                                const QueryPlan& plan,
                                const StructuralAttr* within_sa,
                                CorpusPos seed_p,
                                std::function<bool(std::vector<CorpusPos>&&)> emit,
                                int64_t* within_hint) const {
    size_t n = query.tokens.size();
    int seed_min_rep = std::max(query.tokens[plan.seed].min_repeat, 1); // seed_len=0 is nonsensical
    int seed_max_rep = query.tokens[plan.seed].max_repeat;

    for (int seed_len = seed_min_rep; seed_len <= seed_max_rep; ++seed_len) {
        // Validate the seed span: positions seed_p..seed_p+seed_len-1 must all match
        if (seed_len > 1) {
            CorpusPos end_p = seed_p + seed_len - 1;
            if (end_p >= corpus_.size()) break;
            bool valid = true;
            for (CorpusPos q = seed_p + 1; q <= end_p; ++q) {
                if (!check_conditions(q, query.tokens[plan.seed].conditions)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) break;
        }

        // Partial match vectors are of size 2*n: [0..n-1]=starts, [n..2n-1]=ends
        std::vector<std::vector<CorpusPos>> partial;
        {
            std::vector<CorpusPos> pm(2 * n, NO_HEAD);
            pm[plan.seed] = seed_p;
            pm[n + plan.seed] = seed_p + seed_len - 1;
            partial.push_back(std::move(pm));
        }

        // Expand outward through plan steps
        for (const auto& step : plan.steps) {
            if (step.edge_idx >= query.relations.size())
                throw std::runtime_error("Internal error: query plan edge index out of range");
            RelationType rel = query.relations[step.edge_idx].type;
            const auto& target_cond = query.tokens[step.to].conditions;
            int to_min = query.tokens[step.to].min_repeat;
            int to_max = query.tokens[step.to].max_repeat;
            bool is_negative = (rel == RelationType::NOT_GOVERNS ||
                                rel == RelationType::NOT_GOV_BY);

            std::vector<std::vector<CorpusPos>> new_partial;

            if (is_negative) {
                // Negative relation: keep partial only if NO target matches
                RelationType pos_rel = (rel == RelationType::NOT_GOVERNS)
                                       ? RelationType::GOVERNS
                                       : RelationType::GOVERNED_BY;
                for (const auto& pm : partial) {
                    CorpusPos from_pos = pm[step.from];
                    if (from_pos == NO_HEAD) continue;
                    bool any_match = false;
                    for_each_related(from_pos, pos_rel, step.reversed, [&](CorpusPos r) -> bool {
                        if (r >= 0 && r < corpus_.size() &&
                            check_conditions(r, target_cond)) {
                            any_match = true;
                            return false;  // stop early
                        }
                        return true;
                    });
                    if (!any_match)
                        new_partial.push_back(pm);
                }
            } else if (rel == RelationType::SEQUENCE && (to_min != 1 || to_max != 1)) {
                // SEQUENCE with repetition/optional on the target token
                for (const auto& pm : partial) {
                    CorpusPos from_end = pm[n + step.from];
                    CorpusPos from_start = pm[step.from];
                    if (from_end == NO_HEAD) {
                        // From-token was skipped (optional) — walk to nearest placed token
                        CorpusPos fallback = NO_HEAD;
                        if (!step.reversed) {
                            for (int k = static_cast<int>(step.from) - 1; k >= 0; --k) {
                                if (pm[n + k] != NO_HEAD) { fallback = pm[n + k]; break; }
                            }
                        } else {
                            for (size_t k = step.from + 1; k < n; ++k) {
                                if (pm[k] != NO_HEAD) { fallback = pm[k]; break; }
                            }
                        }
                        if (fallback == NO_HEAD) continue;
                        from_end = fallback;
                        from_start = fallback;
                    }

                    CorpusPos base;
                    if (!step.reversed) {
                        base = from_end + 1;    // span starts right after from's end
                    } else {
                        base = from_start - 1;  // span ends right before from's start
                    }

                    // Optional token: min=0 path (skip this token entirely)
                    if (to_min == 0) {
                        auto skipped = pm;
                        new_partial.push_back(std::move(skipped));
                    }

                    // Try spans of length 1..to_max
                    for (int len = 1; len <= to_max; ++len) {
                        CorpusPos p = step.reversed ? (base - len + 1) : (base + len - 1);
                        if (p < 0 || p >= corpus_.size()) break;
                        if (!check_conditions(p, target_cond)) break;

                        if (len >= std::max(to_min, 1)) {
                            auto extended = pm;
                            if (!step.reversed) {
                                extended[step.to] = base;
                                extended[n + step.to] = base + len - 1;
                            } else {
                                extended[step.to] = base - len + 1;
                                extended[n + step.to] = base;
                            }
                            new_partial.push_back(std::move(extended));
                        }
                    }
                }
            } else {
                // Standard non-repeating path (or non-SEQUENCE relation)
                for (const auto& pm : partial) {
                    CorpusPos from_pos;
                    if (rel == RelationType::SEQUENCE) {
                        from_pos = step.reversed ? pm[step.from] : pm[n + step.from];
                    } else {
                        from_pos = pm[step.from];
                    }
                    if (from_pos == NO_HEAD) {
                        // From-token was skipped (optional with min=0).
                        // Walk to nearest placed token for SEQUENCE fallback.
                        if (rel == RelationType::SEQUENCE) {
                            CorpusPos fallback = NO_HEAD;
                            if (!step.reversed) {
                                for (int k = static_cast<int>(step.from) - 1; k >= 0; --k) {
                                    if (pm[n + k] != NO_HEAD) { fallback = pm[n + k]; break; }
                                }
                            } else {
                                for (size_t k = step.from + 1; k < n; ++k) {
                                    if (pm[k] != NO_HEAD) { fallback = pm[k]; break; }
                                }
                            }
                            if (fallback == NO_HEAD) continue;
                            from_pos = fallback;
                        } else {
                            continue;
                        }
                    }

                    // Optional token: min=0 path (skip)
                    if (to_min == 0 && rel == RelationType::SEQUENCE) {
                        auto skipped = pm;
                        new_partial.push_back(std::move(skipped));
                    }

                    for_each_related(from_pos, rel, step.reversed, [&](CorpusPos r) -> bool {
                        if (r >= 0 && r < corpus_.size() &&
                            check_conditions(r, target_cond)) {
                            auto extended = pm;
                            extended[step.to] = r;
                            extended[n + step.to] = r;
                            new_partial.push_back(std::move(extended));
                        }
                        return true;
                    });
                }
            }

            partial = std::move(new_partial);
            if (partial.empty()) break;
        }

        // Within-clause filter + emit
        bool stop = false;
        for (auto& pm : partial) {
            if (within_sa) {
                CorpusPos anchor = NO_HEAD;
                for (size_t i = 0; i < n; ++i) {
                    if (pm[i] != NO_HEAD) { anchor = pm[i]; break; }
                }
                if (anchor == NO_HEAD) continue;
                // #28: Use cursor-based find_region when hint is available
                int64_t rgn = within_hint
                    ? within_sa->find_region_from(anchor, *within_hint)
                    : within_sa->find_region(anchor);
                if (rgn < 0) continue;
                if (within_hint) *within_hint = rgn;  // advance cursor
                bool ok = true;
                for (size_t i = 0; i < n; ++i) {
                    if (pm[i] == NO_HEAD) continue;
                    if (within_sa->find_region(pm[i]) != rgn) { ok = false; break; }
                    if (pm[n + i] != pm[i] && within_sa->find_region(pm[n + i]) != rgn) { ok = false; break; }
                }
                if (!ok) continue;
            }
            if (!emit(std::move(pm))) { stop = true; break; }
        }
        if (stop) break;
    }
}

// ── expand_one_seed (convenience wrapper for parallel execution) ──────────

std::vector<Match> QueryExecutor::expand_one_seed(const TokenQuery& query,
                                                  const QueryPlan& plan,
                                                  const StructuralAttr* within_sa,
                                                  CorpusPos seed_p) const {
    std::vector<Match> out;
    size_t n = query.tokens.size();

    expand_seed(query, plan, within_sa, seed_p, [&](std::vector<CorpusPos>&& pm) -> bool {
        Match m;
        m.positions.assign(pm.begin(), pm.begin() + n);
        m.span_ends.assign(pm.begin() + n, pm.begin() + 2 * n);
        out.push_back(std::move(m));
        return true;  // always continue (parallel path collects all)
    });

    return out;
}

// ── #16 Source | Target parallel execution ──────────────────────────────

MatchSet QueryExecutor::execute_parallel(const TokenQuery& source_query,
                                         const TokenQuery& target_query,
                                         size_t max_matches,
                                         bool count_total) {
    MatchSet result;
    result.num_tokens = source_query.tokens.size() + target_query.tokens.size();

    MatchSet source_set = execute(source_query, 0, true, 0, 0, 0, 1);
    MatchSet target_set = execute(target_query, 0, true, 0, 0, 0, 1);

    NameIndexMap src_names = build_name_map(source_query);
    NameIndexMap tgt_names = build_name_map(target_query);

    // Apply region filters (e.g. :: match.text_lang="en") to source/target before joining.
    apply_region_filters(source_query, src_names, source_set);
    apply_region_filters(target_query, tgt_names, target_set);

    const auto& filters = source_query.global_alignment_filters;
    for (const auto& s : source_set.matches) {
        for (const auto& t : target_set.matches) {
            bool aligned = true;
            for (const auto& af : filters) {
                CorpusPos p1 = resolve_name(s, src_names, af.name1);
                CorpusPos p2 = resolve_name(t, tgt_names, af.name2);
                if (p1 == NO_HEAD || p2 == NO_HEAD) {
                    aligned = false;
                    break;
                }
                std::string an1 = normalize_attr(af.attr1);
                std::string an2 = normalize_attr(af.attr2);
                if (!corpus_.has_attr(an1) || !corpus_.has_attr(an2)) {
                    aligned = false;
                    break;
                }
                std::string v1(corpus_.attr(an1).value_at(p1));
                std::string v2(corpus_.attr(an2).value_at(p2));
                if (v1 != v2) {
                    aligned = false;
                    break;
                }
            }
            if (aligned) {
                result.parallel_matches.emplace_back(s, t);
                result.total_count++;
                if (max_matches > 0 && result.total_count >= max_matches && !count_total)
                    goto done;
            }
        }
    }
done:
    result.total_exact = true;
    return result;
}

// ── #12 Global filters ───────────────────────────────────────────────────

void QueryExecutor::apply_region_filters(const TokenQuery& query, const NameIndexMap& name_map, MatchSet& result) const {
    if (query.global_region_filters.empty()) return;

    std::vector<Match> kept;
    kept.reserve(result.matches.size());
    for (const auto& m : result.matches) {
        bool pass = true;

        for (const auto& gf : query.global_region_filters) {
            size_t us = gf.region_attr.find('_');
            if (us == std::string::npos || us + 1 >= gf.region_attr.size()) continue;
            std::string struct_name = gf.region_attr.substr(0, us);
            std::string attr_name = gf.region_attr.substr(us + 1);
            if (!corpus_.has_structure(struct_name)) { pass = false; break; }
            const auto& sa = corpus_.structure(struct_name);
            if (!sa.has_region_attr(attr_name)) { pass = false; break; }
            // Resolve position: use anchor token name if set, otherwise match start
            CorpusPos pos = m.first_pos();
            if (!gf.anchor_name.empty()) {
                CorpusPos ap = resolve_name(m, name_map, gf.anchor_name);
                if (ap != NO_HEAD) pos = ap;
            }
            int64_t rgn = sa.find_region(pos);
            if (rgn < 0) { pass = false; break; }
            std::string rval(sa.region_value(attr_name, static_cast<size_t>(rgn)));
            if (!compare_value(gf.op, rval, gf.value)) { pass = false; break; }
        }

        if (pass) kept.push_back(m);
    }

    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

void QueryExecutor::apply_global_filters(const TokenQuery& query, const NameIndexMap& name_map, MatchSet& result) const {
    if (query.global_region_filters.empty() && query.global_alignment_filters.empty())
        return;

    // First apply region filters
    apply_region_filters(query, name_map, result);
    if (query.global_alignment_filters.empty() || result.matches.empty())
        return;

    std::vector<Match> kept;
    kept.reserve(result.matches.size());
    for (const auto& m : result.matches) {
        bool pass = true;

        for (const auto& af : query.global_alignment_filters) {
            CorpusPos p1 = resolve_name(m, name_map, af.name1);
            CorpusPos p2 = resolve_name(m, name_map, af.name2);
            if (p1 == NO_HEAD || p2 == NO_HEAD) { pass = false; break; }
            std::string an1 = normalize_attr(af.attr1);
            std::string an2 = normalize_attr(af.attr2);
            if (!corpus_.has_attr(an1) || !corpus_.has_attr(an2))
                { pass = false; break; }
            std::string v1(corpus_.attr(an1).value_at(p1));
            std::string v2(corpus_.attr(an2).value_at(p2));
            if (v1 != v2) { pass = false; break; }
        }

        if (pass) kept.push_back(m);
    }

    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

// ── Anchor preprocessing and filtering ───────────────────────────────────

TokenQuery QueryExecutor::strip_anchors(const TokenQuery& query,
                                        std::vector<AnchorConstraint>& constraints) {
    TokenQuery cleaned;
    cleaned.within = query.within;
    cleaned.not_within = query.not_within;
    cleaned.within_having = query.within_having;
    cleaned.containing_clauses = query.containing_clauses;
    cleaned.global_region_filters = query.global_region_filters;
    cleaned.global_alignment_filters = query.global_alignment_filters;
    cleaned.position_orders = query.position_orders;

    // Map from old token indices to new (after stripping anchors)
    std::vector<int> old_to_new(query.tokens.size(), -1);

    for (size_t i = 0; i < query.tokens.size(); ++i) {
        if (query.tokens[i].is_anchor()) {
            // Record the constraint: bind to the nearest real token
            AnchorConstraint ac;
            ac.region = query.tokens[i].anchor_region;
            ac.is_start = (query.tokens[i].anchor == RegionAnchorType::REGION_START);
            ac.attrs = query.tokens[i].anchor_attrs;

            if (ac.is_start) {
                // <s> binds to the next real token
                for (size_t j = i + 1; j < query.tokens.size(); ++j) {
                    if (!query.tokens[j].is_anchor()) {
                        // We'll set token_idx after we know the new index
                        ac.token_idx = j;  // temporarily store old index
                        break;
                    }
                }
            } else {
                // </s> binds to the previous real token
                for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
                    if (!query.tokens[static_cast<size_t>(j)].is_anchor()) {
                        ac.token_idx = static_cast<size_t>(j);
                        break;
                    }
                }
            }
            constraints.push_back(ac);
        } else {
            old_to_new[i] = static_cast<int>(cleaned.tokens.size());
            cleaned.tokens.push_back(query.tokens[i]);
        }
    }

    // Remap constraint token indices from old to new
    for (auto& ac : constraints) {
        int new_idx = old_to_new[ac.token_idx];
        if (new_idx < 0) {
            // Anchor binding to another anchor — shouldn't happen, ignore
            ac.token_idx = 0;
        } else {
            ac.token_idx = static_cast<size_t>(new_idx);
        }
    }

    // Rebuild relations: only keep relations between consecutive real tokens
    // The relation between old tokens[i] and tokens[i+1] maps to the relation
    // between the real tokens they connect.
    // For a chain like <s> [] [] </s>, the relations are:
    //   [<s>→[]] [[]→[]] [[]]→</s>]
    // After stripping, we just need the relation between [] and []
    // Use SEQUENCE for all surviving consecutive pairs (original relations carry over)
    for (size_t i = 0; i + 1 < cleaned.tokens.size(); ++i) {
        // Find the original relation between these tokens
        // Default to SEQUENCE if we can't determine from originals
        cleaned.relations.push_back({RelationType::SEQUENCE});
    }

    // Better: try to find original relations between the real tokens
    // Overwrite with actual relation types where possible
    cleaned.relations.clear();
    std::vector<size_t> real_indices;
    for (size_t i = 0; i < query.tokens.size(); ++i) {
        if (!query.tokens[i].is_anchor())
            real_indices.push_back(i);
    }
    for (size_t k = 0; k + 1 < real_indices.size(); ++k) {
        size_t from = real_indices[k];
        size_t to = real_indices[k + 1];
        // Find the relation on the edge closest to 'from' going toward 'to'
        // The original relations[i] connects tokens[i] and tokens[i+1]
        // Between from and to, pick the first non-anchor relation
        RelationType rel = RelationType::SEQUENCE;  // default
        for (size_t e = from; e < to && e < query.relations.size(); ++e) {
            if (!query.tokens[e].is_anchor() || !query.tokens[e + 1].is_anchor()) {
                rel = query.relations[e].type;
                break;
            }
        }
        cleaned.relations.push_back({rel});
    }

    return cleaned;
}

void QueryExecutor::apply_anchor_filters(const std::vector<AnchorConstraint>& constraints,
                                         MatchSet& result) const {
    if (constraints.empty()) return;

    std::vector<Match> kept;
    kept.reserve(result.matches.size());

    for (const auto& m : result.matches) {
        bool pass = true;
        for (const auto& ac : constraints) {
            if (ac.token_idx >= m.positions.size()) { pass = false; break; }
            CorpusPos pos = ac.is_start ? m.positions[ac.token_idx]
                                        : (ac.token_idx < m.span_ends.size()
                                           ? m.span_ends[ac.token_idx]
                                           : m.positions[ac.token_idx]);
            if (!corpus_.has_structure(ac.region)) { pass = false; break; }
            const auto& sa = corpus_.structure(ac.region);
            int64_t rgn = sa.find_region(pos);
            if (rgn < 0) { pass = false; break; }

            Region reg = sa.get(static_cast<size_t>(rgn));
            if (ac.is_start) {
                // Token must be at region start
                if (pos != reg.start) { pass = false; break; }
            } else {
                // Token must be at region end
                if (pos != reg.end) { pass = false; break; }
            }

            // Check optional region attributes: <text genre="book">
            for (const auto& [key, val] : ac.attrs) {
                if (!sa.has_region_attr(key)) { pass = false; break; }
                std::string_view rv = sa.region_value(key, static_cast<size_t>(rgn));
                if (rv != val) { pass = false; break; }
            }
            if (!pass) break;
        }
        if (pass) kept.push_back(m);
    }

    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

void QueryExecutor::apply_within_having(const TokenQuery& query, MatchSet& result) const {
    if (!query.within_having || query.within.empty()) return;
    if (!corpus_.has_structure(query.within)) return;

    const auto& sa = corpus_.structure(query.within);

    std::vector<Match> kept;
    kept.reserve(result.matches.size());

    for (const auto& m : result.matches) {
        CorpusPos pos = m.first_pos();
        int64_t rgn = sa.find_region(pos);
        if (rgn < 0) continue;

        Region reg = sa.get(static_cast<size_t>(rgn));
        CorpusPos rgn_start = reg.start;
        CorpusPos rgn_end   = reg.end;

        // Check if any position in the region satisfies the having condition
        bool found = false;
        for (CorpusPos p = rgn_start; p <= rgn_end; ++p) {
            if (check_conditions(p, query.within_having)) {
                found = true;
                break;
            }
        }
        if (found) kept.push_back(m);
    }

    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

// ── Containing / not-within / position-order filters ─────────────────

void QueryExecutor::apply_containing(const TokenQuery& query, MatchSet& result) const {
    if (query.containing_clauses.empty()) return;

    std::vector<Match> kept;
    kept.reserve(result.matches.size());

    for (const auto& m : result.matches) {
        CorpusPos ms = m.first_pos();
        CorpusPos me = m.last_pos();
        bool pass = true;

        for (const auto& cc : query.containing_clauses) {
            bool found = false;

            if (cc.is_subtree) {
                // Dependency subtree containment: find a token in [ms, me] matching
                // cc.subtree_cond whose full subtree is also within [ms, me].
                if (!corpus_.has_deps()) { found = false; }
                else {
                    const auto& deps = corpus_.deps();
                    for (CorpusPos p = ms; p <= me; ++p) {
                        if (!check_conditions(p, cc.subtree_cond)) continue;
                        auto sub = deps.subtree(p);
                        bool all_inside = true;
                        for (CorpusPos sp : sub) {
                            if (sp < ms || sp > me) { all_inside = false; break; }
                        }
                        if (all_inside) { found = true; break; }
                    }
                }
            } else {
                // Structural region containment: check if any region of the
                // specified type has both start and end within [ms, me].
                if (!corpus_.has_structure(cc.region)) { found = false; }
                else {
                    const auto& sa = corpus_.structure(cc.region);
                    // Binary search: find first region whose start >= ms
                    size_t count = sa.region_count();
                    // Linear scan from the region containing ms
                    int64_t rgn = sa.find_region(ms);
                    if (rgn < 0) rgn = 0;
                    for (size_t r = static_cast<size_t>(rgn); r < count; ++r) {
                        Region reg = sa.get(r);
                        if (reg.start > me) break;  // past match end
                        if (reg.start >= ms && reg.end <= me) {
                            found = true;
                            break;
                        }
                    }
                }
            }

            if (cc.negated) found = !found;
            if (!found) { pass = false; break; }
        }

        if (pass) kept.push_back(m);
    }

    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

void QueryExecutor::apply_not_within(const TokenQuery& query, MatchSet& result) const {
    if (!query.not_within || query.within.empty()) return;
    if (!corpus_.has_structure(query.within)) return;

    const auto& sa = corpus_.structure(query.within);

    std::vector<Match> kept;
    kept.reserve(result.matches.size());

    for (const auto& m : result.matches) {
        // "not within s": match must NOT be entirely inside any region of type s
        bool inside = false;
        for (size_t i = 0; i < m.positions.size(); ++i) {
            if (m.positions[i] == NO_HEAD) continue;
            int64_t rgn = sa.find_region(m.positions[i]);
            if (rgn >= 0) { inside = true; break; }
        }
        if (!inside) kept.push_back(m);
    }

    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

void QueryExecutor::apply_position_orders(const TokenQuery& query, const NameIndexMap& name_map, MatchSet& result) const {
    if (query.position_orders.empty()) return;

    std::vector<Match> kept;
    kept.reserve(result.matches.size());

    for (const auto& m : result.matches) {
        bool pass = true;
        for (const auto& po : query.position_orders) {
            CorpusPos p1 = resolve_name(m, name_map, po.name1);
            CorpusPos p2 = resolve_name(m, name_map, po.name2);
            if (p1 == NO_HEAD || p2 == NO_HEAD) { pass = false; break; }
            bool ok = false;
            switch (po.op) {
                case CompOp::LT:  ok = (p1 < p2); break;
                case CompOp::GT:  ok = (p1 > p2); break;
                default: ok = (p1 < p2); break;
            }
            if (!ok) { pass = false; break; }
        }
        if (pass) kept.push_back(m);
    }

    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

// ── Set operations ──────────────────────────────────────────────────────

std::vector<CorpusPos> QueryExecutor::intersect(
        const std::vector<CorpusPos>& a,
        const std::vector<CorpusPos>& b) {
    std::vector<CorpusPos> out;
    auto ia = a.begin(), ib = b.begin();
    while (ia != a.end() && ib != b.end()) {
        if (*ia == *ib) { out.push_back(*ia); ++ia; ++ib; }
        else if (*ia < *ib) ++ia;
        else ++ib;
    }
    return out;
}

std::vector<CorpusPos> QueryExecutor::unite(
        const std::vector<CorpusPos>& a,
        const std::vector<CorpusPos>& b) {
    std::vector<CorpusPos> out;
    std::merge(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    auto it = std::unique(out.begin(), out.end());
    out.erase(it, out.end());
    return out;
}

} // namespace manatree
