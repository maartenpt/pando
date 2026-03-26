#pragma once

#include "core/types.h"
#include "query/ast.h"
#include "corpus/corpus.h"
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

#include "index/fold_map.h"

#ifdef PANDO_USE_RE2
#include <re2/re2.h>
#else
#include <regex>
#include <mutex>
#endif

namespace manatree {

struct Match {
    std::vector<CorpusPos> positions;    // start position of each query token's span
    std::vector<CorpusPos> span_ends;    // end position (inclusive) of each token's span

    // Overall match extent: min/max across ALL positions and span_ends.
    // Safe for dependency queries where positions may not be in corpus order.
    CorpusPos first_pos() const {
        if (positions.empty()) return 0;
        CorpusPos mn = positions[0];
        for (CorpusPos p : positions)
            if (p != NO_HEAD && p < mn) mn = p;
        return mn;
    }
    CorpusPos last_pos() const {
        if (positions.empty()) return 0;
        // Use span_ends when present, otherwise positions
        CorpusPos mx = 0;
        for (size_t i = 0; i < positions.size(); ++i) {
            CorpusPos end = (!span_ends.empty()) ? span_ends[i] : positions[i];
            if (end != NO_HEAD && end > mx) mx = end;
        }
        return mx;
    }

    // Collect all matched corpus positions into a sorted vector.
    // Used by KWIC display to know which positions are "highlighted".
    std::vector<CorpusPos> matched_positions() const {
        std::vector<CorpusPos> out;
        for (size_t i = 0; i < positions.size(); ++i) {
            if (positions[i] == NO_HEAD) continue;
            CorpusPos end = (!span_ends.empty()) ? span_ends[i] : positions[i];
            for (CorpusPos p = positions[i]; p <= end; ++p)
                out.push_back(p);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
};

// O1: Lightweight name→position resolution. Build once from query, resolve at read time.
using NameIndexMap = std::unordered_map<std::string, size_t>;

inline NameIndexMap build_name_map(const TokenQuery& query) {
    NameIndexMap map;
    for (size_t i = 0; i < query.tokens.size(); ++i)
        if (!query.tokens[i].name.empty())
            map[query.tokens[i].name] = i;
    return map;
}

// Resolve a named token to its position in a match, using the name map.
inline CorpusPos resolve_name(const Match& m, const NameIndexMap& names,
                              const std::string& name) {
    auto it = names.find(name);
    if (it == names.end() || it->second >= m.positions.size()) return NO_HEAD;
    return m.positions[it->second];
}

struct MatchSet {
    std::vector<Match> matches;
    size_t num_tokens = 0;

    size_t total_count = 0;
    bool   total_exact = true;

    // #16: Source | Target: pairs (source_match, target_match) when parallel query
    std::vector<std::pair<Match, Match>> parallel_matches;

    // Debug info (always populated)
    size_t seed_token = 0;
    std::vector<size_t> cardinalities;
};

// Query plan: start from the most selective token, expand outward.
struct QueryPlan {
    size_t seed = 0;
    struct Step {
        size_t from;
        size_t to;
        size_t edge_idx;
        bool   reversed;
    };
    std::vector<Step> steps;
    std::vector<size_t> cardinalities;
};

class QueryExecutor {
public:
    explicit QueryExecutor(const Corpus& corpus);

    // max_total_cap: when count_total and cap > 0, stop counting at cap and set total_exact=false
    // sample_size > 0: reservoir sample this many random matches (ignores max_matches for output).
    // random_seed: for reproducible --sample (default 0 uses time-based seed when sampling).
    // num_threads > 1: process seed positions in parallel (multi-token queries only; materializes seeds).
    MatchSet execute(const TokenQuery& query,
                     size_t max_matches = 0,
                     bool count_total = false,
                     size_t max_total_cap = 0,
                     size_t sample_size = 0,
                     uint32_t random_seed = 0,
                     unsigned num_threads = 1);

    // #16: Source | Target: run source and target queries, join on source_query.global_alignment_filters.
    // Returns MatchSet with parallel_matches filled; matches is empty.
    MatchSet execute_parallel(const TokenQuery& source_query,
                              const TokenQuery& target_query,
                              size_t max_matches = 0,
                              bool count_total = false);

private:
    // ── Cardinality estimation (O(1) per EQ condition via rev.idx) ──────

    size_t estimate_cardinality(const ConditionPtr& cond) const;
    size_t estimate_leaf(const AttrCondition& ac) const;

    // ── Query planning ──────────────────────────────────────────────────

    QueryPlan plan_query(const TokenQuery& query) const;

    // ── Per-position condition checking (avoids materializing sets) ─────

    bool check_conditions(CorpusPos pos, const ConditionPtr& cond) const;
    bool check_leaf(CorpusPos pos, const AttrCondition& ac) const;

    // ── Relation traversal from a single position ───────────────────────

    // Given a position on one side of a relation, return positions on the
    // other side.  `reversed` means we're traversing the edge backward
    // (from the right token to the left token in the original query).
    std::vector<CorpusPos> find_related(CorpusPos pos,
                                        RelationType rel,
                                        bool reversed) const;

    // Callback-based variant: avoids heap allocation for SEQUENCE (the common case).
    // Calls f(related_pos) for each related position; f returns false to stop early.
    template<typename F>
    void for_each_related(CorpusPos pos, RelationType rel, bool reversed, F&& f) const {
        if (rel == RelationType::SEQUENCE) {
            if (reversed) {
                if (pos > 0) f(pos - 1);
            } else {
                if (pos + 1 < corpus_.size()) f(pos + 1);
            }
        } else {
            auto v = find_related(pos, rel, reversed);
            for (CorpusPos r : v)
                if (!f(r)) break;
        }
    }

    // ── Seed resolution (uses inverted index for the starting token) ────

    std::vector<CorpusPos> resolve_conditions(const ConditionPtr& cond) const;
    std::vector<CorpusPos> resolve_leaf(const AttrCondition& ac) const;

    // Lazy iteration: call f(pos) for each matching position; return false from f to stop.
    void for_each_seed_position(const ConditionPtr& cond,
                                std::function<bool(CorpusPos)> f) const;

    // Expand one seed position through all plan steps and within-clause filter.
    // Calls emit(pm) for each complete partial match vector (size 2*n).
    // emit returns true to continue expanding, false to stop early.
    // within_hint: cursor for find_region_from (#28); updated on return.
    void expand_seed(const TokenQuery& query, const QueryPlan& plan,
                     const StructuralAttr* within_sa, CorpusPos seed_p,
                     std::function<bool(std::vector<CorpusPos>&&)> emit,
                     int64_t* within_hint = nullptr) const;

    // Expand one seed position to all full matches (for parallel execution).
    // Convenience wrapper around expand_seed that builds Match objects.
    std::vector<Match> expand_one_seed(const TokenQuery& query,
                                       const QueryPlan& plan,
                                       const StructuralAttr* within_sa,
                                       CorpusPos seed_p) const;

    // #25: Pre-resolve EQ string values to LexiconIds for integer comparison in check_leaf.
    void compile_conditions(const ConditionPtr& cond) const;
    void compile_query(const TokenQuery& query) const;

    std::string normalize_attr(const std::string& attr) const;

    static std::vector<CorpusPos> intersect(const std::vector<CorpusPos>& a,
                                            const std::vector<CorpusPos>& b);
    static std::vector<CorpusPos> unite(const std::vector<CorpusPos>& a,
                                        const std::vector<CorpusPos>& b);

    // #12: Apply global filters (name_map built from query via build_name_map)
    void apply_region_filters(const TokenQuery& query, const NameIndexMap& name_map, MatchSet& result) const;
    void apply_global_filters(const TokenQuery& query, const NameIndexMap& name_map, MatchSet& result) const;

    // Region anchor constraint (from <s>, </s> in query)
    struct AnchorConstraint {
        size_t token_idx;          // index of the real token this anchor binds to
        std::string region;        // region name (e.g. "s")
        bool is_start;             // true = token must be at region start, false = at region end
        std::vector<std::pair<std::string, std::string>> attrs;  // optional attr constraints
    };

    // Strip anchor tokens from a query, returning the cleaned query and constraints
    static TokenQuery strip_anchors(const TokenQuery& query,
                                    std::vector<AnchorConstraint>& constraints);

    // Apply anchor constraints as post-filter
    void apply_anchor_filters(const std::vector<AnchorConstraint>& constraints,
                              MatchSet& result) const;

    // Apply within-having as post-filter
    void apply_within_having(const TokenQuery& query, MatchSet& result) const;

    // Apply containing/not-containing clauses as post-filter
    void apply_containing(const TokenQuery& query, MatchSet& result) const;

    // Apply not-within as post-filter (inverts within)
    void apply_not_within(const TokenQuery& query, MatchSet& result) const;

    // Apply positional ordering constraints (:: a < b)
    void apply_position_orders(const TokenQuery& query, const NameIndexMap& name_map, MatchSet& result) const;

    const Corpus& corpus_;

    // Fold map cache: keyed by "attr:mode" where mode is "lc", "noacc", "lcnoacc"
    const FoldMap& get_fold_map(const std::string& attr, bool case_fold, bool accent_fold) const;
    mutable std::unordered_map<std::string, FoldMap> fold_map_cache_;
    mutable std::mutex fold_map_mutex_;

#ifdef PANDO_USE_RE2
    // RE2 objects are thread-safe for matching once constructed.
    // Only the cache insertion needs synchronization (handled by mutable + unique_ptr).
    mutable std::unordered_map<std::string, std::unique_ptr<re2::RE2>> regex_cache_;
    mutable std::mutex regex_cache_mutex_;  // protects cache insertion only
#else
    mutable std::unordered_map<std::string, std::regex> regex_cache_;
    mutable std::mutex regex_cache_mutex_;
#endif
};

} // namespace manatree
