#include "index/fold_map.h"
#include <algorithm>
#include <cctype>

namespace manatree {

const std::vector<LexiconId> FoldMap::empty_;

std::string FoldMap::to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    // Simple ASCII lowercase; UTF-8 multi-byte bytes (high bit set) pass through.
    // For full Unicode lowercasing, integrate ICU or a lightweight lib.
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z')
            out += static_cast<char>(c + 32);
        else
            out += static_cast<char>(c);
    }
    return out;
}

std::string FoldMap::strip_accents(std::string_view s) {
    // Lightweight ASCII diacritics stripping for common Latin-1 accented chars
    // encoded in UTF-8 (two-byte sequences starting with 0xC3).
    // For full Unicode, integrate ICU NFD + combining mark removal.
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            // Map common accented chars to base letters (Latin-1 Supplement in UTF-8)
            // À-Ö (0xC0-0xD6) → A-O, Ø-ß (0xD8-0xDF) → O-s, à-ö (0xE0-0xF6) → a-o, ø-ÿ (0xF8-0xFF) → o-y
            if (c2 >= 0x80 && c2 <= 0x85) { out += 'A'; i += 2; continue; }  // À Á Â Ã Ä Å
            if (c2 == 0x86) { out += "AE"; i += 2; continue; }                 // Æ
            if (c2 == 0x87) { out += 'C'; i += 2; continue; }                  // Ç
            if (c2 >= 0x88 && c2 <= 0x8B) { out += 'E'; i += 2; continue; }  // È É Ê Ë
            if (c2 >= 0x8C && c2 <= 0x8F) { out += 'I'; i += 2; continue; }  // Ì Í Î Ï
            if (c2 == 0x90) { out += 'D'; i += 2; continue; }                  // Ð
            if (c2 == 0x91) { out += 'N'; i += 2; continue; }                  // Ñ
            if (c2 >= 0x92 && c2 <= 0x96) { out += 'O'; i += 2; continue; }  // Ò Ó Ô Õ Ö
            if (c2 == 0x98) { out += 'O'; i += 2; continue; }                  // Ø
            if (c2 >= 0x99 && c2 <= 0x9C) { out += 'U'; i += 2; continue; }  // Ù Ú Û Ü
            if (c2 == 0x9D) { out += 'Y'; i += 2; continue; }                  // Ý
            if (c2 >= 0xA0 && c2 <= 0xA5) { out += 'a'; i += 2; continue; }  // à á â ã ä å
            if (c2 == 0xA6) { out += "ae"; i += 2; continue; }                 // æ
            if (c2 == 0xA7) { out += 'c'; i += 2; continue; }                  // ç
            if (c2 >= 0xA8 && c2 <= 0xAB) { out += 'e'; i += 2; continue; }  // è é ê ë
            if (c2 >= 0xAC && c2 <= 0xAF) { out += 'i'; i += 2; continue; }  // ì í î ï
            if (c2 == 0xB0) { out += 'd'; i += 2; continue; }                  // ð
            if (c2 == 0xB1) { out += 'n'; i += 2; continue; }                  // ñ
            if (c2 >= 0xB2 && c2 <= 0xB6) { out += 'o'; i += 2; continue; }  // ò ó ô õ ö
            if (c2 == 0xB8) { out += 'o'; i += 2; continue; }                  // ø
            if (c2 >= 0xB9 && c2 <= 0xBC) { out += 'u'; i += 2; continue; }  // ù ú û ü
            if (c2 == 0xBD || c2 == 0xBF) { out += 'y'; i += 2; continue; }  // ý ÿ
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

std::string FoldMap::to_lower_no_accents(std::string_view s) {
    return to_lower(strip_accents(s));
}

FoldMap FoldMap::build_lowercase(const Lexicon& lex) {
    FoldMap fm;
    LexiconId n = lex.size();
    for (LexiconId id = 0; id < n; ++id) {
        std::string folded = to_lower(lex.get(id));
        fm.map_[folded].push_back(id);
    }
    return fm;
}

FoldMap FoldMap::build_no_accents(const Lexicon& lex) {
    FoldMap fm;
    LexiconId n = lex.size();
    for (LexiconId id = 0; id < n; ++id) {
        std::string folded = strip_accents(lex.get(id));
        fm.map_[folded].push_back(id);
    }
    return fm;
}

FoldMap FoldMap::build_lc_no_accents(const Lexicon& lex) {
    FoldMap fm;
    LexiconId n = lex.size();
    for (LexiconId id = 0; id < n; ++id) {
        std::string folded = to_lower_no_accents(lex.get(id));
        fm.map_[folded].push_back(id);
    }
    return fm;
}

} // namespace manatree
