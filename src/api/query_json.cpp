#include "api/query_json.h"
#include "query/parser.h"
#include <sstream>
#include <chrono>

namespace manatree {

namespace {

static std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static std::string jstr(std::string_view s) {
    return "\"" + json_escape(s) + "\"";
}

struct KwicContext {
    std::string left, match, right;
};

static KwicContext build_context(const Corpus& corpus, const Match& m, int ctx_width) {
    const auto& form = corpus.attr("form");
    KwicContext ctx;
    CorpusPos first = m.first_pos();
    CorpusPos last  = m.last_pos();
    CorpusPos left_start = std::max(CorpusPos(0), first - ctx_width);
    for (CorpusPos p = left_start; p < first; ++p) {
        if (!ctx.left.empty()) ctx.left += ' ';
        ctx.left += std::string(form.value_at(p));
    }
    // Iterate through all positions in all spans
    for (size_t i = 0; i < m.positions.size(); ++i) {
        if (m.positions[i] == NO_HEAD) continue;
        CorpusPos span_end = (!m.span_ends.empty()) ? m.span_ends[i] : m.positions[i];
        for (CorpusPos p = m.positions[i]; p <= span_end; ++p) {
            if (!ctx.match.empty()) ctx.match += ' ';
            ctx.match += std::string(form.value_at(p));
        }
    }
    CorpusPos right_end = std::min(corpus.size() - 1, last + ctx_width);
    for (CorpusPos p = last + 1; p <= right_end; ++p) {
        if (!ctx.right.empty()) ctx.right += ' ';
        ctx.right += std::string(form.value_at(p));
    }
    return ctx;
}

static std::string_view lookup_doc_id(const Corpus& corpus, CorpusPos pos) {
    if (!corpus.has_structure("text")) return {};
    const auto& text = corpus.structure("text");
    if (!text.has_values()) return {};
    int64_t ri = text.find_region(pos);
    if (ri < 0) return {};
    return text.region_value(static_cast<size_t>(ri));
}

} // namespace

std::pair<MatchSet, double> run_single_query(const Corpus& corpus,
                                            const std::string& query_text,
                                            const QueryOptions& opts) {
    Parser parser(query_text);
    Program prog = parser.parse();
    if (prog.empty() || !prog[0].has_query)
        return {MatchSet{}, 0.0};

    QueryExecutor executor(corpus);
    size_t max_m = opts.offset + opts.limit;
    bool count_t = opts.total;
    size_t max_total_cap = (opts.total && opts.max_total > 0) ? opts.max_total : 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    MatchSet ms = executor.execute(prog[0].query, max_m, count_t, max_total_cap);
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {std::move(ms), elapsed};
}

std::string to_query_result_json(const Corpus& corpus,
                                 const std::string& query_text,
                                 const MatchSet& ms,
                                 const QueryOptions& opts,
                                 double elapsed_ms) {
    std::ostringstream out;
    size_t stored = ms.matches.size();
    size_t start = std::min(opts.offset, stored);
    size_t end   = std::min(start + opts.limit, stored);
    size_t returned = end - start;

    out << "{\n";
    out << "  \"ok\": true,\n";
    out << "  \"backend\": \"manatree\",\n";
    out << "  \"operation\": \"query\",\n";
    out << "  \"result\": {\n";
    out << "    \"query\": {\"language\": \"clickcql\", \"text\": " << jstr(query_text) << "},\n";
    out << "    \"page\": {\"start\": " << start << ", \"size\": " << opts.limit
        << ", \"returned\": " << returned << ", \"total\": " << ms.total_count
        << ", \"total_exact\": " << (ms.total_exact ? "true" : "false") << "},\n";
    out << "    \"hits\": [\n";

    for (size_t i = start; i < end; ++i) {
        const auto& m = ms.matches[i];
        CorpusPos match_start = m.first_pos();
        CorpusPos match_end   = m.last_pos();
        auto doc_id = lookup_doc_id(corpus, match_start);
        auto ctx = build_context(corpus, m, opts.context);

        if (i > start) out << ",\n";
        out << "      {";
        out << "\"doc_id\": " << (doc_id.empty() ? "null" : jstr(doc_id));
        out << ", \"match_start\": " << match_start << ", \"match_end\": " << match_end;
        out << ", \"context\": {\"left\": " << jstr(ctx.left)
            << ", \"match\": " << jstr(ctx.match) << ", \"right\": " << jstr(ctx.right) << "}";
        out << ", \"tokens\": [";
        const auto& attr_names = opts.attrs.empty()
            ? corpus.attr_names() : opts.attrs;
        bool first_tok = true;
        for (size_t t = 0; t < m.positions.size(); ++t) {
            if (m.positions[t] == NO_HEAD) continue;
            CorpusPos span_end = (!m.span_ends.empty()) ? m.span_ends[t] : m.positions[t];
            for (CorpusPos p = m.positions[t]; p <= span_end; ++p) {
                if (!first_tok) out << ", ";
                first_tok = false;
                out << "{\"pos\": " << p;
                for (const auto& attr_name : attr_names) {
                    if (!corpus.has_attr(attr_name)) continue;
                    auto val = corpus.attr(attr_name).value_at(p);
                    if (val == "_") continue;
                    out << ", " << jstr(attr_name) << ": " << jstr(val);
                }
                out << "}";
            }
        }
        out << "]}";
    }
    out << "\n    ]";

    if (opts.debug) {
        out << ",\n    \"debug\": {\n";
        out << "      \"corpus_size\": " << corpus.size() << ",\n";
        out << "      \"has_deps\": " << (corpus.has_deps() ? "true" : "false") << ",\n";
        out << "      \"elapsed_ms\": " << elapsed_ms << ",\n";
        out << "      \"seed_token\": " << ms.seed_token << ",\n";
        out << "      \"cardinalities\": [";
        for (size_t i = 0; i < ms.cardinalities.size(); ++i) {
            if (i > 0) out << ", ";
            out << ms.cardinalities[i];
        }
        out << "]\n    }";
    }
    out << "\n  }\n}\n";
    return out.str();
}

std::string to_info_json(const Corpus& corpus) {
    std::ostringstream out;
    out << "{\n  \"ok\": true,\n  \"operation\": \"info\",\n";
    out << "  \"result\": {\n";
    out << "    \"size\": " << corpus.size() << ",\n";
    out << "    \"has_deps\": " << (corpus.has_deps() ? "true" : "false") << ",\n";
    out << "    \"attributes\": [";
    const auto& names = corpus.attr_names();
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out << ", ";
        out << jstr(names[i]);
    }
    out << "]\n  }\n}\n";
    return out.str();
}

} // namespace manatree
