#include "index/lexicon.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace manatree {

// ── Lexicon (read-only) ─────────────────────────────────────────────────

void Lexicon::open(const std::string& base, bool preload) {
    strings_ = MmapFile::open(base + ".lex", preload);
    offsets_ = MmapFile::open(base + ".lex.idx", preload);
}

LexiconId Lexicon::size() const {
    if (!offsets_.valid()) return 0;
    // offsets has lex_size+1 entries
    return static_cast<LexiconId>(offsets_.count<int64_t>()) - 1;
}

std::string_view Lexicon::get(LexiconId id) const {
    if (id < 0 || id >= size()) return {};
    const auto* off = offsets_.as<int64_t>();
    const char* base = static_cast<const char*>(strings_.data());
    return std::string_view(base + off[id],
                            static_cast<size_t>(off[id + 1] - off[id] - 1));
}

LexiconId Lexicon::lookup(std::string_view value) const {
    LexiconId lo = 0, hi = size();
    while (lo < hi) {
        LexiconId mid = lo + (hi - lo) / 2;
        std::string_view entry = get(mid);
        int cmp = value.compare(entry);
        if (cmp == 0) return mid;
        if (cmp < 0) hi = mid;
        else         lo = mid + 1;
    }
    return UNKNOWN_LEX;
}

// ── LexiconBuilder ──────────────────────────────────────────────────────

void LexiconBuilder::observe(const std::string& value) {
    seen_.insert(value);
}

void LexiconBuilder::finalize() {
    sorted_.assign(seen_.begin(), seen_.end());
    std::sort(sorted_.begin(), sorted_.end());
    id_map_.clear();
    id_map_.reserve(sorted_.size());
    for (LexiconId i = 0; i < static_cast<LexiconId>(sorted_.size()); ++i)
        id_map_[sorted_[i]] = i;
    finalized_ = true;
}

LexiconId LexiconBuilder::lookup(const std::string& value) const {
    if (!finalized_)
        throw std::logic_error("LexiconBuilder::lookup called before finalize");
    auto it = id_map_.find(value);
    return it != id_map_.end() ? it->second : UNKNOWN_LEX;
}

void LexiconBuilder::write(const std::string& base) const {
    if (!finalized_)
        throw std::logic_error("LexiconBuilder::write called before finalize");
    std::vector<int64_t> offsets;
    write_strings(base + ".lex", sorted_, offsets);
    write_vec(base + ".lex.idx", offsets);
}

} // namespace manatree
