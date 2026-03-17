#include "index/positional_attr.h"
#include <algorithm>
#include <stdexcept>

namespace manatree {

// ── PositionalAttr (read-only) ──────────────────────────────────────────

void PositionalAttr::open(const std::string& base, CorpusPos corpus_size, bool preload) {
    corpus_size_ = corpus_size;
    lexicon_.open(base, preload);
    corpus_  = MmapFile::open(base + ".dat", preload);
    rev_     = MmapFile::open(base + ".rev", preload);
    rev_idx_ = MmapFile::open(base + ".rev.idx", preload);

    // Infer .dat element width from file size
    if (corpus_size_ > 0) {
        dat_width_ = static_cast<int>(corpus_.size() / static_cast<size_t>(corpus_size_));
        if (dat_width_ != 1 && dat_width_ != 2 && dat_width_ != 4)
            throw std::runtime_error("Invalid .dat element width (" +
                                     std::to_string(dat_width_) + ") for " + base);
    }

    // Infer .rev element width from file size
    if (rev_.size() > 0 && corpus_size_ > 0) {
        rev_width_ = static_cast<int>(rev_.size() / static_cast<size_t>(corpus_size_));
        if (rev_width_ != 2 && rev_width_ != 4 && rev_width_ != 8)
            throw std::runtime_error("Invalid .rev element width (" +
                                     std::to_string(rev_width_) + ") for " + base);
    }
}

LexiconId PositionalAttr::id_at(CorpusPos pos) const {
    switch (dat_width_) {
        case 1: return static_cast<LexiconId>(corpus_.as<uint8_t>()[pos]);
        case 2: return static_cast<LexiconId>(corpus_.as<uint16_t>()[pos]);
        default: return corpus_.as<int32_t>()[pos];
    }
}

std::string_view PositionalAttr::value_at(CorpusPos pos) const {
    return lexicon_.get(id_at(pos));
}

size_t PositionalAttr::count_of(const std::string& value) const {
    LexiconId id = lexicon_.lookup(value);
    if (id == UNKNOWN_LEX) return 0;
    return count_of_id(id);
}

size_t PositionalAttr::count_of_id(LexiconId id) const {
    const auto* idx = rev_idx_.as<int64_t>();
    return static_cast<size_t>(idx[id + 1] - idx[id]);
}

std::vector<CorpusPos> PositionalAttr::positions_of(const std::string& value) const {
    LexiconId id = lexicon_.lookup(value);
    if (id == UNKNOWN_LEX) return {};
    return positions_of_id(id);
}

std::vector<CorpusPos> PositionalAttr::positions_of_id(LexiconId id) const {
    const auto* idx = rev_idx_.as<int64_t>();
    int64_t start = idx[id];
    int64_t end   = idx[id + 1];
    size_t count = static_cast<size_t>(end - start);

    std::vector<CorpusPos> result(count);

    switch (rev_width_) {
        case 2: {
            auto* p = rev_.as<int16_t>() + start;
            for (size_t i = 0; i < count; ++i) result[i] = static_cast<CorpusPos>(p[i]);
            break;
        }
        case 4: {
            auto* p = rev_.as<int32_t>() + start;
            for (size_t i = 0; i < count; ++i) result[i] = static_cast<CorpusPos>(p[i]);
            break;
        }
        default: {
            auto* p = rev_.as<int64_t>() + start;
            for (size_t i = 0; i < count; ++i) result[i] = p[i];
            break;
        }
    }
    return result;
}

std::vector<CorpusPos> PositionalAttr::positions_matching(
        const std::regex& re) const {
    std::vector<CorpusPos> result;
    LexiconId n = lexicon_.size();
    for (LexiconId id = 0; id < n; ++id) {
        std::string_view sv = lexicon_.get(id);
        std::string s(sv);
        if (std::regex_match(s, re)) {
            auto span = positions_of_id(id);
            result.insert(result.end(), span.begin(), span.end());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<CorpusPos> PositionalAttr::positions_not(
        const std::string& value, CorpusPos csz) const {
    LexiconId exclude = lexicon_.lookup(value);
    if (exclude == UNKNOWN_LEX) {
        std::vector<CorpusPos> all(static_cast<size_t>(csz));
        for (CorpusPos i = 0; i < csz; ++i) all[static_cast<size_t>(i)] = i;
        return all;
    }
    auto excluded = positions_of_id(exclude);
    std::vector<CorpusPos> result;
    result.reserve(static_cast<size_t>(csz) - excluded.size());
    size_t ei = 0;
    for (CorpusPos p = 0; p < csz; ++p) {
        if (ei < excluded.size() && excluded[ei] == p) { ++ei; continue; }
        result.push_back(p);
    }
    return result;
}

// ── PositionalAttrBuilder ───────────────────────────────────────────────

void PositionalAttrBuilder::observe(CorpusPos pos, const std::string& value) {
    if (static_cast<size_t>(pos) >= values_.size())
        values_.resize(static_cast<size_t>(pos) + 1);
    values_[static_cast<size_t>(pos)] = value;
    lex_builder_.observe(value);
}

void PositionalAttrBuilder::build_and_write(const std::string& dir) {
    lex_builder_.finalize();

    std::string base = dir + "/" + name_;
    lex_builder_.write(base);

    CorpusPos n = static_cast<CorpusPos>(values_.size());
    LexiconId lex_sz = lex_builder_.size();

    // Write .dat (corpus array: position → lex id, stored as int32 LexiconId)
    std::vector<LexiconId> corpus_data(static_cast<size_t>(n));
    for (CorpusPos p = 0; p < n; ++p)
        corpus_data[static_cast<size_t>(p)] = lex_builder_.lookup(values_[static_cast<size_t>(p)]);
    write_vec(base + ".dat", corpus_data);

    // Build inverted index: lex_id → sorted positions
    std::vector<std::vector<CorpusPos>> rev_lists(static_cast<size_t>(lex_sz));
    for (CorpusPos p = 0; p < n; ++p)
        rev_lists[static_cast<size_t>(corpus_data[static_cast<size_t>(p)])].push_back(p);

    // Write .rev (int64 positions) and .rev.idx (int64 offsets)
    std::vector<int64_t> rev_idx;
    rev_idx.reserve(static_cast<size_t>(lex_sz) + 1);
    std::vector<CorpusPos> rev_flat;
    rev_flat.reserve(static_cast<size_t>(n));

    int64_t offset = 0;
    for (LexiconId id = 0; id < lex_sz; ++id) {
        rev_idx.push_back(offset);
        const auto& list = rev_lists[static_cast<size_t>(id)];
        rev_flat.insert(rev_flat.end(), list.begin(), list.end());
        offset += static_cast<int64_t>(list.size());
    }
    rev_idx.push_back(offset);

    write_vec(base + ".rev", rev_flat);
    write_vec(base + ".rev.idx", rev_idx);
}

} // namespace manatree
