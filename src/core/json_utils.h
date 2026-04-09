#pragma once

// Shared JSON helpers used by query_main.cpp, query_json.cpp, and server_main.cpp.

#include "core/types.h"
#include "query/executor.h"
#include "corpus/corpus.h"
#include <string>
#include <string_view>
#include <cstdio>

namespace pando {

inline std::string json_escape(std::string_view s) {
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

inline std::string jstr(std::string_view s) {
    return "\"" + json_escape(s) + "\"";
}

struct KwicContext {
    std::string left;
    std::string match;
    std::string right;
};

inline KwicContext build_context(const Corpus& corpus, const Match& m, int ctx_width) {
    const auto& form = corpus.attr("form");
    KwicContext ctx;

    CorpusPos first = m.first_pos();
    CorpusPos last  = m.last_pos();

    CorpusPos left_start = std::max(CorpusPos(0), first - ctx_width);
    for (CorpusPos p = left_start; p < first; ++p) {
        if (!ctx.left.empty()) ctx.left += ' ';
        ctx.left += form.value_at(p);
    }

    // Match text: full span from first to last in corpus order.
    // For discontinuous matches this includes gap tokens; the JSON
    // "tokens" array has the per-token detail for consumers that need it.
    for (CorpusPos p = first; p <= last; ++p) {
        if (!ctx.match.empty()) ctx.match += ' ';
        ctx.match += form.value_at(p);
    }

    CorpusPos right_end = std::min(corpus.size() - 1, last + ctx_width);
    for (CorpusPos p = last + 1; p <= right_end; ++p) {
        if (!ctx.right.empty()) ctx.right += ' ';
        ctx.right += form.value_at(p);
    }

    return ctx;
}

inline std::string_view lookup_doc_id(const Corpus& corpus, CorpusPos pos) {
    if (!corpus.has_structure("text")) return {};
    const auto& text = corpus.structure("text");
    if (!text.has_values()) return {};
    int64_t ri = text.find_region(pos);
    if (ri < 0) return {};
    return text.region_value(static_cast<size_t>(ri));
}

} // namespace pando
