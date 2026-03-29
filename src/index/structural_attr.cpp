#include "index/structural_attr.h"
#include <algorithm>
#include <cstddef>
#include <fstream>

namespace manatree {

void StructuralAttr::open(const std::string& rgn_path, bool preload) {
    file_ = MmapFile::open(rgn_path, preload);
    region_attrs_.clear();
    region_value_rev_.clear();
    region_attr_names_.clear();

    // Try to load optional default region values (.val + .val.idx)
    std::string base = rgn_path.substr(0, rgn_path.size() - 4); // strip .rgn
    std::string val_path = base + ".val";
    std::string idx_path = base + ".val.idx";
    std::ifstream probe(val_path);
    if (probe.good()) {
        val_     = MmapFile::open(val_path, preload);
        val_idx_ = MmapFile::open(idx_path, preload);
    }
}

void StructuralAttr::add_region_attr(const std::string& attr_name,
                                    const std::string& val_path,
                                    const std::string& idx_path,
                                    bool preload) {
    MmapFile v = MmapFile::open(val_path, preload);
    MmapFile i = MmapFile::open(idx_path, preload);
    region_attrs_[attr_name] = std::make_pair(std::move(v), std::move(i));
    region_attr_names_.push_back(attr_name);

    std::string base = val_path;
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".val") == 0)
        base.resize(base.size() - 4);
    else
        return;
    std::ifstream probe(base + ".rev.idx");
    if (!probe.good()) return;
    probe.close();

    RegionValueRev rv;
    rv.lex.open(base, preload);
    rv.rev = MmapFile::open(base + ".rev", preload);
    rv.rev_idx = MmapFile::open(base + ".rev.idx", preload);
    if (rv.rev.valid() && rv.rev_idx.valid())
        region_value_rev_[attr_name] = std::move(rv);
}

bool StructuralAttr::has_region_attr(const std::string& attr_name) const {
    return region_attrs_.count(attr_name) != 0;
}

std::string_view StructuralAttr::region_value(const std::string& attr_name, size_t idx) const {
    auto it = region_attrs_.find(attr_name);
    if (it == region_attrs_.end()) return {};
    const MmapFile& v = it->second.first;
    const MmapFile& i = it->second.second;
    if (!v.valid() || !i.valid()) return {};
    const auto* off = i.as<int64_t>();
    if (idx + 1 >= i.count<int64_t>()) return {};
    const char* base = static_cast<const char*>(v.data());
    return std::string_view(base + off[idx],
                            static_cast<size_t>(off[idx + 1] - off[idx] - 1));
}

size_t StructuralAttr::region_count() const {
    return file_.count<Region>();
}

Region StructuralAttr::get(size_t idx) const {
    return file_.as<Region>()[idx];
}

int64_t StructuralAttr::find_region(CorpusPos pos) const {
    size_t n = region_count();
    if (n == 0) return -1;
    const Region* r = file_.as<Region>();

    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (r[mid].start <= pos) lo = mid + 1;
        else                     hi = mid;
    }
    if (lo == 0) return -1;
    --lo;
    if (pos >= r[lo].start && pos <= r[lo].end) return static_cast<int64_t>(lo);
    return -1;
}

int64_t StructuralAttr::find_region_from(CorpusPos pos, int64_t hint) const {
    size_t n = region_count();
    if (n == 0) return -1;
    const Region* r = file_.as<Region>();

    // If hint is valid, try linear advance from there
    if (hint >= 0 && static_cast<size_t>(hint) < n) {
        size_t h = static_cast<size_t>(hint);
        // Check if pos is still in the hinted region
        if (pos >= r[h].start && pos <= r[h].end) return hint;
        // Advance forward (pos > r[h].end means we moved past this region)
        if (pos > r[h].end) {
            // Scan forward a few regions (bounded to avoid degenerate cases)
            for (size_t i = h + 1; i < n && i <= h + 8; ++i) {
                if (pos < r[i].start) return -1;  // gap between regions
                if (pos <= r[i].end) return static_cast<int64_t>(i);
            }
        }
    }
    // Fallback to full binary search
    return find_region(pos);
}

bool StructuralAttr::same_region(CorpusPos a, CorpusPos b) const {
    int64_t ra = find_region(a);
    return ra >= 0 && ra == find_region(b);
}

std::string_view StructuralAttr::region_value(size_t idx) const {
    if (!val_.valid() || !val_idx_.valid()) return {};
    const auto* off = val_idx_.as<int64_t>();
    const char* base = static_cast<const char*>(val_.data());
    return std::string_view(base + off[idx],
                            static_cast<size_t>(off[idx + 1] - off[idx] - 1));
}

bool StructuralAttr::has_region_value_reverse(const std::string& attr_name) const {
    return region_value_rev_.count(attr_name) != 0;
}

size_t StructuralAttr::count_regions_with_attr_eq(const std::string& attr_name,
                                                    const std::string& value) const {
    auto it = region_value_rev_.find(attr_name);
    if (it == region_value_rev_.end()) return SIZE_MAX;
    const RegionValueRev& rv = it->second;
    LexiconId id = rv.lex.lookup(value);
    if (id == UNKNOWN_LEX) return 0;
    const auto* idx = rv.rev_idx.as<int64_t>();
    return static_cast<size_t>(idx[static_cast<size_t>(id) + 1] - idx[static_cast<size_t>(id)]);
}

bool StructuralAttr::region_matches_attr_eq_rev(const std::string& attr_name, size_t region_idx,
                                                  const std::string& value) const {
    auto it = region_value_rev_.find(attr_name);
    if (it == region_value_rev_.end()) return false;
    const RegionValueRev& rv = it->second;
    LexiconId vid = rv.lex.lookup(value);
    if (vid == UNKNOWN_LEX) return false;
    const auto* idx = rv.rev_idx.as<int64_t>();
    size_t vi = static_cast<size_t>(vid);
    if (vi + 1 >= rv.rev_idx.count<int64_t>()) return false;
    int64_t lo = idx[vi];
    int64_t hi = idx[vi + 1];
    const int64_t* p = rv.rev.as<int64_t>() + lo;
    size_t n = static_cast<size_t>(hi - lo);
    int64_t key = static_cast<int64_t>(region_idx);
    return std::binary_search(p, p + n, key);
}

size_t StructuralAttr::token_span_sum_for_attr_eq(const std::string& attr_name,
                                                    const std::string& value) const {
    auto it = region_value_rev_.find(attr_name);
    if (it == region_value_rev_.end()) return SIZE_MAX;
    const RegionValueRev& rv = it->second;
    LexiconId vid = rv.lex.lookup(value);
    if (vid == UNKNOWN_LEX) return 0;
    const auto* idx = rv.rev_idx.as<int64_t>();
    size_t vi = static_cast<size_t>(vid);
    if (vi + 1 >= rv.rev_idx.count<int64_t>()) return 0;
    int64_t lo = idx[vi];
    int64_t hi = idx[vi + 1];
    const int64_t* p = rv.rev.as<int64_t>() + lo;
    size_t sum = 0;
    for (int64_t k = 0; k < hi - lo; ++k) {
        size_t ri = static_cast<size_t>(p[k]);
        Region r = get(ri);
        sum += static_cast<size_t>(r.end - r.start + 1);
    }
    return sum;
}

} // namespace manatree
