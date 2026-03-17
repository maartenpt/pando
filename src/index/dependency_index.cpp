#include "index/dependency_index.h"
#include <stdexcept>
#include <algorithm>

namespace manatree {

// ── DependencyIndex (read-only) ─────────────────────────────────────────

void DependencyIndex::open(const std::string& dir,
                           const StructuralAttr& sentences,
                           bool preload) {
    sentences_       = &sentences;
    head_file_       = MmapFile::open(dir + "/dep.head", preload);
    euler_in_file_   = MmapFile::open(dir + "/dep.euler_in", preload);
    euler_out_file_  = MmapFile::open(dir + "/dep.euler_out", preload);
}

CorpusPos DependencyIndex::head(CorpusPos pos) const {
    // Defensive bounds check to avoid OOB access on malformed queries/plans
    size_t n = head_file_.size() / sizeof(int16_t);
    if (pos < 0 || static_cast<size_t>(pos) >= n)
        return NO_HEAD;

    int16_t local = head_file_.as<int16_t>()[pos];
    if (local == -1) return NO_HEAD;
    int64_t ri = sentences_->find_region(pos);
    if (ri < 0) return NO_HEAD;
    Region sent = sentences_->get(static_cast<size_t>(ri));
    return sent.start + static_cast<CorpusPos>(local);
}

std::vector<CorpusPos> DependencyIndex::children(CorpusPos pos) const {
    int64_t ri = sentences_->find_region(pos);
    if (ri < 0) return {};
    Region sent = sentences_->get(static_cast<size_t>(ri));
    int16_t my_local = static_cast<int16_t>(pos - sent.start);

    std::vector<CorpusPos> result;
    const int16_t* heads = head_file_.as<int16_t>();
    for (CorpusPos p = sent.start; p <= sent.end; ++p) {
        if (heads[p] == my_local)
            result.push_back(p);
    }
    return result;
}

std::vector<CorpusPos> DependencyIndex::subtree(CorpusPos pos) const {
    int64_t ri = sentences_->find_region(pos);
    if (ri < 0) return {};
    Region sent = sentences_->get(static_cast<size_t>(ri));
    int16_t my_local = static_cast<int16_t>(pos - sent.start);
    int sent_len = static_cast<int>(sent.end - sent.start + 1);

    // One scan to build local children map
    const int16_t* heads = head_file_.as<int16_t>();
    std::vector<std::vector<int16_t>> ch(static_cast<size_t>(sent_len));
    for (int i = 0; i < sent_len; ++i) {
        int16_t h = heads[sent.start + i];
        if (h >= 0)
            ch[static_cast<size_t>(h)].push_back(static_cast<int16_t>(i));
    }

    // DFS from my_local
    std::vector<CorpusPos> result;
    std::vector<int16_t> stack(ch[static_cast<size_t>(my_local)].begin(),
                               ch[static_cast<size_t>(my_local)].end());
    while (!stack.empty()) {
        int16_t cur = stack.back();
        stack.pop_back();
        result.push_back(sent.start + static_cast<CorpusPos>(cur));
        for (int16_t c : ch[static_cast<size_t>(cur)])
            stack.push_back(c);
    }
    return result;
}

std::vector<CorpusPos> DependencyIndex::ancestors(CorpusPos pos) const {
    std::vector<CorpusPos> result;
    CorpusPos cur = head(pos);
    while (cur != NO_HEAD) {
        result.push_back(cur);
        cur = head(cur);
    }
    return result;
}

int16_t DependencyIndex::euler_in(CorpusPos pos) const {
    return euler_in_file_.as<int16_t>()[pos];
}

int16_t DependencyIndex::euler_out(CorpusPos pos) const {
    return euler_out_file_.as<int16_t>()[pos];
}

bool DependencyIndex::is_ancestor(CorpusPos anc, CorpusPos desc) const {
    int64_t sa = sentences_->find_region(anc);
    int64_t sd = sentences_->find_region(desc);
    if (sa != sd) return false;
    return euler_in(anc) < euler_in(desc) && euler_out(anc) > euler_out(desc);
}

// ── DependencyIndexBuilder ──────────────────────────────────────────────

void DependencyIndexBuilder::set_corpus_size(CorpusPos n) {
    head_abs_.assign(static_cast<size_t>(n), NO_HEAD);
}

void DependencyIndexBuilder::set_head(CorpusPos pos, CorpusPos head_pos) {
    head_abs_[static_cast<size_t>(pos)] = head_pos;
}

void DependencyIndexBuilder::dfs_local(
        CorpusPos sent_start, int16_t local_id,
        const std::vector<std::vector<int16_t>>& children,
        int16_t& clock) {
    CorpusPos pos = sent_start + static_cast<CorpusPos>(local_id);
    euler_in_[static_cast<size_t>(pos)] = clock++;
    for (int16_t c : children[static_cast<size_t>(local_id)])
        dfs_local(sent_start, c, children, clock);
    euler_out_[static_cast<size_t>(pos)] = clock++;
}

void DependencyIndexBuilder::finalize(const std::vector<Region>& sentences) {
    size_t n = head_abs_.size();
    head_local_.resize(n);
    euler_in_.resize(n, 0);
    euler_out_.resize(n, 0);

    for (const auto& sent : sentences) {
        int sent_len = static_cast<int>(sent.end - sent.start + 1);

        // Convert absolute heads to sentence-local int16
        for (CorpusPos p = sent.start; p <= sent.end; ++p) {
            CorpusPos h = head_abs_[static_cast<size_t>(p)];
            head_local_[static_cast<size_t>(p)] =
                (h == NO_HEAD) ? int16_t(-1)
                               : static_cast<int16_t>(h - sent.start);
        }

        // Build per-sentence children lists for Euler tour
        std::vector<std::vector<int16_t>> children(
            static_cast<size_t>(sent_len));
        for (int i = 0; i < sent_len; ++i) {
            int16_t h = head_local_[static_cast<size_t>(sent.start + i)];
            if (h >= 0)
                children[static_cast<size_t>(h)].push_back(
                    static_cast<int16_t>(i));
        }
        // Sort children by position for deterministic Euler tour
        for (auto& ch : children)
            std::sort(ch.begin(), ch.end());

        // DFS from root(s), counter resets per sentence
        int16_t clock = 0;
        for (int i = 0; i < sent_len; ++i) {
            if (head_local_[static_cast<size_t>(sent.start + i)] == -1)
                dfs_local(sent.start, static_cast<int16_t>(i),
                          children, clock);
        }
    }
}

void DependencyIndexBuilder::write(const std::string& dir) const {
    write_vec(dir + "/dep.head", head_local_);
    write_vec(dir + "/dep.euler_in", euler_in_);
    write_vec(dir + "/dep.euler_out", euler_out_);
}

} // namespace manatree
