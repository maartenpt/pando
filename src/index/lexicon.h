#pragma once

#include "core/types.h"
#include "core/mmap_file.h"
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace manatree {

// Read-only lexicon backed by mmap'd .lex and .lex.idx files.
// Supports O(log V) string→ID lookup via binary search on sorted strings.
class Lexicon {
public:
    void open(const std::string& base_path, bool preload = false);

    LexiconId lookup(std::string_view value) const;
    std::string_view get(LexiconId id) const;
    LexiconId size() const;

private:
    MmapFile strings_;       // .lex
    MmapFile offsets_;       // .lex.idx  (int64 byte offsets, size = lex_size+1)
};

// Collects unique strings during corpus building, then sorts and writes.
class LexiconBuilder {
public:
    void observe(const std::string& value);
    void finalize();

    LexiconId lookup(const std::string& value) const;
    LexiconId size() const { return static_cast<LexiconId>(sorted_.size()); }

    void write(const std::string& base_path) const;

private:
    std::unordered_set<std::string> seen_;
    std::vector<std::string> sorted_;
    std::unordered_map<std::string, LexiconId> id_map_;
    bool finalized_ = false;
};

} // namespace manatree
