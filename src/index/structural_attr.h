#pragma once

#include "core/types.h"
#include "core/mmap_file.h"
#include "index/lexicon.h"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace manatree {

// Read-only structural attribute: stores (start, end) pairs for regions
// (sentences, paragraphs, documents) and supports O(log N) position-to-region
// lookup via binary search on sorted start positions.
//
// Optionally has per-region string values: one default (.val/.val.idx) and/or
// multiple named attributes (e.g. text_id.val, text_year.val) for #8 region attrs.
class StructuralAttr {
public:
    void open(const std::string& rgn_path, bool preload = false);

    // Add a named region attribute (e.g. "id" for text_id.val). Call after open().
    void add_region_attr(const std::string& attr_name,
                         const std::string& val_path,
                         const std::string& idx_path,
                         bool preload = false);

    size_t region_count() const;
    Region get(size_t idx) const;

    int64_t find_region(CorpusPos pos) const;

    // #28: Cursor-based find_region — start search from hint index.
    // When iterating sorted positions, pass the last returned region index
    // as hint; advances linearly from there, falling back to binary search
    // when the hint is stale. Returns -1 if pos is not in any region.
    int64_t find_region_from(CorpusPos pos, int64_t hint) const;

    bool same_region(CorpusPos a, CorpusPos b) const;

    bool has_values() const { return val_.valid(); }
    std::string_view region_value(size_t idx) const;

    // Named region attributes (#8): e.g. region_value("id", idx) for text_id.
    bool has_region_attr(const std::string& attr_name) const;
    std::string_view region_value(const std::string& attr_name, size_t idx) const;
    const std::vector<std::string>& region_attr_names() const { return region_attr_names_; }

    // Optional reverse index (pando-index): value → sorted region ids in .rev / .lex next to .val.
    bool has_region_value_reverse(const std::string& attr_name) const;
    // O(1) count of regions with this attribute value; 0 if unknown value; SIZE_MAX if no rev index.
    size_t count_regions_with_attr_eq(const std::string& attr_name, const std::string& value) const;

    // When a reverse index exists: O(log K) check that region_idx is in the posting for value.
    // If value is not in the .lex or region_idx is not listed, returns false.
    bool region_matches_attr_eq_rev(const std::string& attr_name, size_t region_idx,
                                   const std::string& value) const;

    // Sum of inclusive token spans for regions with attr == value. 0 if value not in .lex.
    // SIZE_MAX if there is no reverse index for this attr (caller uses a conservative bound).
    size_t token_span_sum_for_attr_eq(const std::string& attr_name, const std::string& value) const;

private:
    struct RegionValueRev {
        Lexicon lex;       // distinct values (.lex)
        MmapFile rev;      // int64_t region indices
        MmapFile rev_idx;  // cumulative offsets per lex id
    };
    MmapFile file_;       // .rgn: Region pairs
    MmapFile val_;        // .val: default (unnamed) region value strings
    MmapFile val_idx_;    // .val.idx: int64 byte offsets into .val
    std::unordered_map<std::string, std::pair<MmapFile, MmapFile>> region_attrs_;
    std::unordered_map<std::string, RegionValueRev> region_value_rev_;
    std::vector<std::string> region_attr_names_;
};

} // namespace manatree
