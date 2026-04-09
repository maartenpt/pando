#include "core/count_hierarchy_json.h"
#include "core/json_utils.h"
#include <algorithm>
#include <string_view>

namespace pando {

namespace {

struct CountTree {
    std::map<std::string, CountTree> children;
    size_t leaf_count = 0;
};

static size_t subtree_total(const CountTree& t) {
    size_t s = t.leaf_count;
    for (const auto& [k, ch] : t.children)
        s += subtree_total(ch);
    return s;
}

static void add_path(CountTree& t, const std::vector<std::string>& path, size_t idx, size_t count) {
    if (idx >= path.size()) return;
    if (idx + 1 == path.size())
        t.children[path[idx]].leaf_count += count;
    else
        add_path(t.children[path[idx]], path, idx + 1, count);
}

static void split_key(std::string_view k, std::vector<std::string>& parts) {
    parts.clear();
    size_t start = 0;
    for (size_t i = 0; i <= k.size(); ++i) {
        if (i == k.size() || k[i] == '\t') {
            parts.emplace_back(k.substr(start, i - start));
            start = i + 1;
        }
    }
}

static void emit_level(std::ostream& out, const CountTree& node, size_t depth,
                       const std::vector<std::string>& fields, size_t total, double parent_total,
                       size_t group_limit, bool is_root, int indent, bool& first) {
    if (depth >= fields.size()) return;

    std::vector<std::pair<std::string, size_t>> order;
    order.reserve(node.children.size());
    for (const auto& [k, ch] : node.children)
        order.push_back({k, subtree_total(ch)});
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    size_t n = order.size();
    if (is_root && group_limit > 0 && group_limit < n)
        n = group_limit;

    for (size_t i = 0; i < n; ++i) {
        const std::string& key = order[i].first;
        const CountTree& ch = node.children.at(key);
        size_t subtotal = subtree_total(ch);
        double pct = total > 0 ? 100.0 * static_cast<double>(subtotal) / static_cast<double>(total) : 0.0;
        double pct_of_parent =
            parent_total > 0 ? 100.0 * static_cast<double>(subtotal) / parent_total : 0.0;

        if (!first) out << ",\n";
        first = false;
        for (int s = 0; s < indent; ++s) out << ' ';

        if (depth + 1 < fields.size()) {
            out << "{\"field\": " << jstr(fields[depth]) << ", \"value\": " << jstr(key)
                << ", \"count\": " << subtotal << ", \"pct\": " << pct;
            if (depth > 0)
                out << ", \"pct_of_parent\": " << pct_of_parent;
            out << ", \"children\": [";
            bool inner_first = true;
            emit_level(out, ch, depth + 1, fields, total, static_cast<double>(subtotal), 0, false,
                       indent + 2, inner_first);
            out << "\n";
            for (int s = 0; s < indent; ++s) out << ' ';
            out << "]}";
        } else {
            out << "{\"field\": " << jstr(fields[depth]) << ", \"value\": " << jstr(key)
                << ", \"count\": " << subtotal << ", \"pct\": " << pct;
            if (depth > 0)
                out << ", \"pct_of_parent\": " << pct_of_parent;
            out << "}";
        }
    }
}

} // namespace

void emit_count_result_hierarchy_json(std::ostream& out,
                                      const std::vector<std::string>& fields,
                                      const std::map<std::string, size_t>& counts,
                                      size_t total,
                                      size_t group_limit) {
    CountTree root;
    std::vector<std::string> parts;
    for (const auto& [k, cnt] : counts) {
        split_key(k, parts);
        if (parts.size() != fields.size()) continue;
        add_path(root, parts, 0, cnt);
    }

    out << "  \"hierarchy\": [\n";
    bool first = true;
    emit_level(out, root, 0, fields, total, static_cast<double>(total), group_limit, true, 4, first);
    out << "\n  ]";
}

} // namespace pando
