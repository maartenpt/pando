#include "query/executor.h"

namespace pando {

// ── Anchor preprocessing and filtering ───────────────────────────────────

namespace {
static bool region_span_contains(const Region& outer, const Region& inner) {
    return outer.start <= inner.start && outer.end >= inner.end;
}

static std::optional<Region> named_region_span(const Corpus& corpus, const Match& m,
                                               const std::string& name) {
    auto it = m.named_regions.find(name);
    if (it == m.named_regions.end()) return std::nullopt;
    if (!corpus.has_structure(it->second.struct_name)) return std::nullopt;
    const auto& sa = corpus.structure(it->second.struct_name);
    return sa.get(it->second.region_idx);
}
} // namespace

TokenQuery QueryExecutor::strip_anchors(const TokenQuery& query,
                                        std::vector<AnchorConstraint>& constraints) {
    TokenQuery cleaned;
    cleaned.within = query.within;
    cleaned.not_within = query.not_within;
    cleaned.within_having = query.within_having;
    cleaned.containing_clauses = query.containing_clauses;
    cleaned.global_region_filters = query.global_region_filters;
    cleaned.global_alignment_filters = query.global_alignment_filters;
    cleaned.global_function_filters = query.global_function_filters;
    cleaned.position_orders = query.position_orders;

    std::vector<int> old_to_new(query.tokens.size(), -1);
    for (size_t i = 0; i < query.tokens.size(); ++i) {
        if (query.tokens[i].is_anchor()) {
            AnchorConstraint ac;
            ac.region = query.tokens[i].anchor_region;
            ac.is_start = (query.tokens[i].anchor == RegionAnchorType::REGION_START);
            ac.attrs = query.tokens[i].anchor_attrs;
            ac.binding_name = query.tokens[i].name;
            ac.anchor_region_clauses = query.tokens[i].anchor_region_clauses;

            bool bound = false;
            if (ac.is_start) {
                for (size_t j = i + 1; j < query.tokens.size(); ++j) {
                    if (!query.tokens[j].is_anchor()) { ac.token_idx = j; bound = true; break; }
                }
            } else {
                for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
                    if (!query.tokens[static_cast<size_t>(j)].is_anchor()) {
                        ac.token_idx = static_cast<size_t>(j);
                        bound = true;
                        break;
                    }
                }
            }
            if (bound) {
                constraints.push_back(ac);
            } else if (ac.is_start && !ac.binding_name.empty()) {
                ac.region_enumeration = true;
                constraints.push_back(ac);
            }
        } else {
            old_to_new[i] = static_cast<int>(cleaned.tokens.size());
            cleaned.tokens.push_back(query.tokens[i]);
        }
    }

    for (auto& ac : constraints) {
        if (ac.region_enumeration) continue;
        int new_idx = old_to_new[ac.token_idx];
        ac.token_idx = (new_idx < 0) ? 0u : static_cast<size_t>(new_idx);
    }

    cleaned.relations.clear();
    std::vector<size_t> real_indices;
    for (size_t i = 0; i < query.tokens.size(); ++i) {
        if (!query.tokens[i].is_anchor()) real_indices.push_back(i);
    }
    for (size_t k = 0; k + 1 < real_indices.size(); ++k) {
        size_t from = real_indices[k], to = real_indices[k + 1];
        RelationType rel = RelationType::SEQUENCE;
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

namespace {
struct AnchorRowCheck {
    const Corpus& corpus;
    const StructuralAttr& sa;
    const std::string& region_name;
    const std::vector<std::pair<std::string, std::string>>& attrs;
    const std::vector<AnchorRegionClause>& anchor_region_clauses;
    const Match& m;
    bool attrs_match(size_t ri) const {
        for (const auto& [key, val] : attrs) {
            auto rk = resolve_region_attr_key(sa, region_name, key);
            if (!rk) return false;
            if (sa.region_value(*rk, ri) != val) return false;
        }
        return true;
    }
    bool clauses_ok(size_t ri) const {
        Region cur = sa.get(ri);
        for (const auto& cl : anchor_region_clauses) {
            if (cl.kind == AnchorRegionClauseKind::RchildOf) {
                auto pit = m.named_regions.find(cl.peer_label);
                if (pit == m.named_regions.end()) return false;
                const RegionRef& pr = pit->second;
                if (pr.struct_name != region_name) return false;
                if (!sa.has_parent_region_id()) return false;
                int32_t par = sa.parent_region_id(ri);
                if (par < 0 || static_cast<size_t>(par) != pr.region_idx) return false;
            } else if (cl.kind == AnchorRegionClauseKind::RcontainsOf) {
                auto pit = m.named_regions.find(cl.peer_label);
                if (pit == m.named_regions.end()) return false;
                const RegionRef& pr = pit->second;
                if (pr.struct_name != region_name) return false;
                if (!sa.has_parent_region_id()) return false;
                if (!sa.region_is_ancestor_of(ri, pr.region_idx)) return false;
            } else if (cl.kind == AnchorRegionClauseKind::Contains) {
                auto inner = named_region_span(corpus, m, cl.peer_label);
                if (!inner) return false;
                if (!region_span_contains(cur, *inner)) return false;
            }
        }
        return true;
    }
};
} // namespace

size_t QueryExecutor::expand_anchor_constraints(
        const Match& base,
        const std::vector<AnchorConstraint>& constraints,
        const std::function<bool(Match&)>& emit) const {
    size_t emitted = 0;
    bool stop = false;
    std::function<void(size_t, Match&)> recurse = [&](size_t idx, Match& m) {
        if (stop) return;
        if (idx == constraints.size()) {
            Match out = m;
            ++emitted;
            if (!emit(out)) stop = true;
            return;
        }
        const auto& ac = constraints[idx];
        if (ac.region_enumeration) { recurse(idx + 1, m); return; }
        if (ac.token_idx >= m.positions.size()) return;
        CorpusPos pos = ac.is_start ? m.positions[ac.token_idx]
                                    : (ac.token_idx < m.span_ends.size()
                                       ? m.span_ends[ac.token_idx]
                                       : m.positions[ac.token_idx]);
        if (!corpus_.has_structure(ac.region)) return;
        const auto& sa = corpus_.structure(ac.region);
        AnchorRowCheck chk{corpus_, sa, ac.region, ac.attrs, ac.anchor_region_clauses, m};
        const bool multi = corpus_.is_nested(ac.region) || corpus_.is_overlapping(ac.region)
                           || corpus_.is_zerowidth(ac.region);

        auto try_bind = [&](size_t ri) {
            if (stop) return;
            if (!chk.attrs_match(ri) || !chk.clauses_ok(ri)) return;
            if (!ac.binding_name.empty()) {
                auto prev = m.named_regions.find(ac.binding_name);
                bool had = prev != m.named_regions.end();
                RegionRef saved{};
                if (had) saved = prev->second;
                m.named_regions[ac.binding_name] = RegionRef{ac.region, ri};
                recurse(idx + 1, m);
                if (had) m.named_regions[ac.binding_name] = saved;
                else m.named_regions.erase(ac.binding_name);
            } else {
                recurse(idx + 1, m);
            }
        };

        if (!multi) {
            int64_t rgn = sa.find_region(pos);
            if (rgn < 0) return;
            Region reg = sa.get(static_cast<size_t>(rgn));
            if (ac.is_start ? (pos != reg.start) : (pos != reg.end)) return;
            try_bind(static_cast<size_t>(rgn));
            return;
        }

        if (anchor_binding_mode_ == AnchorBindingMode::Innermost) {
            int64_t best_ri = -1;
            Region best_reg{};
            auto consider = [&](size_t ri) -> bool {
                if (!chk.attrs_match(ri) || !chk.clauses_ok(ri)) return true;
                Region cur = sa.get(ri);
                if (best_ri < 0) { best_ri = static_cast<int64_t>(ri); best_reg = cur; return true; }
                bool better = false;
                if (ac.is_start) {
                    if (cur.end < best_reg.end) better = true;
                    else if (cur.end == best_reg.end && static_cast<int64_t>(ri) < best_ri) better = true;
                } else {
                    if (cur.start > best_reg.start) better = true;
                    else if (cur.start == best_reg.start && static_cast<int64_t>(ri) < best_ri) better = true;
                }
                if (better) { best_ri = static_cast<int64_t>(ri); best_reg = cur; }
                return true;
            };
            if (ac.is_start) sa.for_each_region_starting_at(pos, consider);
            else             sa.for_each_region_ending_at(pos, consider);
            if (best_ri >= 0) try_bind(static_cast<size_t>(best_ri));
            return;
        }

        std::vector<size_t> cands;
        auto collect = [&](size_t ri) -> bool { cands.push_back(ri); return true; };
        if (ac.is_start) sa.for_each_region_starting_at(pos, collect);
        else             sa.for_each_region_ending_at(pos, collect);
        for (size_t ri : cands) {
            if (stop) break;
            try_bind(ri);
        }
    };

    Match cur = base;
    cur.named_regions.clear();
    recurse(0, cur);
    return emitted;
}

bool QueryExecutor::resolve_anchor_constraints(Match& m,
                                               const std::vector<AnchorConstraint>& constraints) const {
    if (constraints.empty()) return true;
    bool ok = false;
    Match captured;
    expand_anchor_constraints(m, constraints, [&](Match& r) {
        captured = std::move(r);
        ok = true;
        return false;
    });
    if (!ok) return false;
    m.named_regions = std::move(captured.named_regions);
    return true;
}

void QueryExecutor::apply_anchor_filters(const std::vector<AnchorConstraint>& constraints,
                                         MatchSet& result) const {
    if (constraints.empty()) return;
    std::vector<Match> kept;
    kept.reserve(result.matches.size());
    for (const auto& m : result.matches) {
        expand_anchor_constraints(m, constraints, [&](Match& r) {
            kept.push_back(std::move(r));
            return true;
        });
    }
    result.matches = std::move(kept);
    result.total_count = result.matches.size();
}

} // namespace pando
