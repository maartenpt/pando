#include "corpus/streaming_builder.h"
#include <future>
#include <thread>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace manatree {
namespace fs = std::filesystem;

static constexpr size_t IO_CHUNK = 4 * 1024 * 1024; // 4M elements per chunk

// ── AttrState ───────────────────────────────────────────────────────────

int32_t StreamingBuilder::AttrState::get_or_assign(const std::string& value) {
    auto [it, inserted] = str_to_id.try_emplace(
        value, static_cast<int32_t>(id_to_str.size()));
    if (inserted)
        id_to_str.push_back(value);
    return it->second;
}

int32_t StreamingBuilder::AttrState::get_placeholder() {
    if (placeholder_id < 0)
        placeholder_id = get_or_assign("_");
    return placeholder_id;
}

StreamingBuilder::AttrState::~AttrState() {
    if (dat_file) fclose(dat_file);
}

// ── StreamingBuilder lifecycle ──────────────────────────────────────────

StreamingBuilder::StreamingBuilder(const std::string& output_dir)
    : output_dir_(output_dir) {
    fs::create_directories(output_dir);
    sent_rgn_file_ = fopen((output_dir + "/s.rgn").c_str(), "wb");
    if (!sent_rgn_file_)
        throw std::runtime_error("Cannot create " + output_dir + "/s.rgn");
}

StreamingBuilder::~StreamingBuilder() {
    if (dep_head_file_)      fclose(dep_head_file_);
    if (dep_euler_in_file_)  fclose(dep_euler_in_file_);
    if (dep_euler_out_file_) fclose(dep_euler_out_file_);
    if (sent_rgn_file_)      fclose(sent_rgn_file_);
}

// ── Attribute management ────────────────────────────────────────────────

void StreamingBuilder::ensure_attr(const std::string& name) {
    if (attr_set_.count(name)) return;
    attr_set_.insert(name);
    attr_order_.push_back(name);

    auto state = std::make_unique<AttrState>();
    std::string path = output_dir_ + "/" + name + ".dat";
    state->dat_file = fopen(path.c_str(), "wb");
    if (!state->dat_file)
        throw std::runtime_error("Cannot create " + path);

    // Pre-size hash map: large for form/lemma, small for categorical attrs
    bool large_vocab = (name == "form" || name == "lemma");
    state->str_to_id.reserve(large_vocab ? 500000 : 256);
    state->id_to_str.reserve(large_vocab ? 500000 : 256);

    attrs_[name] = std::move(state);
}

void StreamingBuilder::backfill_attr(AttrState& state) {
    if (state.written >= corpus_size_) return;
    int32_t placeholder = state.get_or_assign("_");
    CorpusPos gap = corpus_size_ - state.written;

    std::vector<int32_t> fill(std::min(static_cast<size_t>(gap), IO_CHUNK),
                              placeholder);
    CorpusPos remaining = gap;
    while (remaining > 0) {
        size_t batch = std::min(static_cast<size_t>(remaining), fill.size());
        fwrite(fill.data(), sizeof(int32_t), batch, state.dat_file);
        remaining -= static_cast<CorpusPos>(batch);
    }
    state.written = corpus_size_;
}

// ── Dependency file management ──────────────────────────────────────────

void StreamingBuilder::open_dep_files() {
    dep_head_file_      = fopen((output_dir_ + "/dep.head").c_str(), "wb");
    dep_euler_in_file_  = fopen((output_dir_ + "/dep.euler_in").c_str(), "wb");
    dep_euler_out_file_ = fopen((output_dir_ + "/dep.euler_out").c_str(), "wb");

    if (!dep_head_file_ || !dep_euler_in_file_ || !dep_euler_out_file_)
        throw std::runtime_error("Cannot create dep files in " + output_dir_);

    // Backfill positions before the first sentence with dep info
    if (dep_written_ < sent_start_) {
        int16_t neg1 = -1, zero = 0;
        for (CorpusPos i = dep_written_; i < sent_start_; ++i) {
            fwrite(&neg1, sizeof(int16_t), 1, dep_head_file_);
            fwrite(&zero, sizeof(int16_t), 1, dep_euler_in_file_);
            fwrite(&zero, sizeof(int16_t), 1, dep_euler_out_file_);
        }
        dep_written_ = sent_start_;
    }
}

// ── add_token ───────────────────────────────────────────────────────────

void StreamingBuilder::add_token(
        const std::unordered_map<std::string, std::string>& attrs,
        int sentence_head_id) {
    if (finalized_)
        throw std::logic_error("add_token called after finalize");

    for (const auto& [name, value] : attrs)
        ensure_attr(name);

    // Write values for attributes present in this token
    for (const auto& [name, value] : attrs) {
        auto& state = *attrs_[name];
        backfill_attr(state);
        int32_t id = state.get_or_assign(value);
        fwrite(&id, sizeof(int32_t), 1, state.dat_file);
        ++state.written;
    }

    // Write placeholder "_" for attributes NOT in this token (fast path)
    for (auto& [name, state] : attrs_) {
        if (state->written > corpus_size_) continue;  // already written above
        backfill_attr(*state);
        int32_t id = state->get_placeholder();
        fwrite(&id, sizeof(int32_t), 1, state->dat_file);
        ++state->written;
    }

    sent_buf_.push_back({sentence_head_id});
    ++corpus_size_;
}

// ── end_sentence ────────────────────────────────────────────────────────

void StreamingBuilder::end_sentence() {
    if (sent_buf_.empty()) return;

    int sent_len = static_cast<int>(sent_buf_.size());
    CorpusPos sent_end = corpus_size_ - 1;

    // Write sentence region
    Region rgn{sent_start_, sent_end};
    fwrite(&rgn, sizeof(Region), 1, sent_rgn_file_);

    // Check if any token has dependency info
    bool any_deps = false;
    for (const auto& t : sent_buf_)
        if (t.sentence_head_id >= 0) { any_deps = true; break; }

    if (any_deps) {
        has_deps_ = true;
        if (!dep_head_file_) open_dep_files();

        // Convert 1-based head IDs to sentence-local int16
        std::vector<int16_t> heads(sent_len);
        for (int i = 0; i < sent_len; ++i) {
            int h = sent_buf_[i].sentence_head_id;
            heads[i] = (h == 0) ? int16_t(-1) : static_cast<int16_t>(h - 1);
        }

        // Build children lists for Euler tour
        std::vector<std::vector<int16_t>> children(sent_len);
        for (int i = 0; i < sent_len; ++i) {
            if (heads[i] >= 0)
                children[static_cast<size_t>(heads[i])].push_back(
                    static_cast<int16_t>(i));
        }
        for (auto& ch : children)
            std::sort(ch.begin(), ch.end());

        // Iterative DFS for Euler tour timestamps
        std::vector<int16_t> euler_in(sent_len, 0);
        std::vector<int16_t> euler_out(sent_len, 0);
        int16_t clock = 0;

        struct DfsFrame { int16_t node; size_t child_idx; };
        std::vector<DfsFrame> stack;

        for (int i = 0; i < sent_len; ++i) {
            if (heads[i] != -1) continue;
            euler_in[i] = clock++;
            stack.push_back({static_cast<int16_t>(i), 0});

            while (!stack.empty()) {
                auto& frame = stack.back();
                auto& ch = children[static_cast<size_t>(frame.node)];
                if (frame.child_idx < ch.size()) {
                    int16_t child = ch[frame.child_idx++];
                    euler_in[child] = clock++;
                    stack.push_back({child, 0});
                } else {
                    euler_out[frame.node] = clock++;
                    stack.pop_back();
                }
            }
        }

        fwrite(heads.data(), sizeof(int16_t), sent_len, dep_head_file_);
        fwrite(euler_in.data(), sizeof(int16_t), sent_len, dep_euler_in_file_);
        fwrite(euler_out.data(), sizeof(int16_t), sent_len, dep_euler_out_file_);
        dep_written_ += sent_len;

    } else if (dep_head_file_) {
        // Dep files exist but this sentence has no deps — write placeholders
        int16_t neg1 = -1, zero = 0;
        for (int i = 0; i < sent_len; ++i) {
            fwrite(&neg1, sizeof(int16_t), 1, dep_head_file_);
            fwrite(&zero, sizeof(int16_t), 1, dep_euler_in_file_);
            fwrite(&zero, sizeof(int16_t), 1, dep_euler_out_file_);
        }
        dep_written_ += sent_len;
    }

    sent_buf_.clear();
    sent_start_ = corpus_size_;
}

// ── add_region ──────────────────────────────────────────────────────────

void StreamingBuilder::add_region(const std::string& type,
                                  CorpusPos start, CorpusPos end) {
    struct_set_.insert(type);
    regions_[type].push_back({start, end});
}

void StreamingBuilder::add_region(const std::string& type,
                                  CorpusPos start, CorpusPos end,
                                  const std::string& value) {
    add_region(type, start, end);
    region_values_[type].push_back(value);
}

void StreamingBuilder::add_region(const std::string& type,
                                  CorpusPos start, CorpusPos end,
                                  const std::vector<std::pair<std::string, std::string>>& attrs) {
    add_region(type, start, end);
    for (const auto& [k, v] : attrs)
        region_attr_values_[type][k].push_back(v);
}

// ── finalize ────────────────────────────────────────────────────────────

void StreamingBuilder::finalize() {
    if (finalized_) return;

    // Auto-close last sentence
    if (!sent_buf_.empty())
        end_sentence();

    // Close streaming files
    for (auto& [name, state] : attrs_) {
        backfill_attr(*state);
        if (state->dat_file) { fclose(state->dat_file); state->dat_file = nullptr; }
    }
    if (dep_head_file_)      { fclose(dep_head_file_);      dep_head_file_ = nullptr; }
    if (dep_euler_in_file_)  { fclose(dep_euler_in_file_);  dep_euler_in_file_ = nullptr; }
    if (dep_euler_out_file_) { fclose(dep_euler_out_file_); dep_euler_out_file_ = nullptr; }
    if (sent_rgn_file_)      { fclose(sent_rgn_file_);      sent_rgn_file_ = nullptr; }

    // Sort attribute names for deterministic output
    std::vector<std::string> sorted_attrs(attr_order_.begin(), attr_order_.end());
    std::sort(sorted_attrs.begin(), sorted_attrs.end());

    std::cerr << "Finalizing " << sorted_attrs.size() << " attributes, "
              << corpus_size_ << " tokens ...\n";

    // Parallel finalization: each attribute is independent (ROADMAP 3a).
    std::vector<std::future<void>> futures;
    futures.reserve(sorted_attrs.size());
    for (const auto& name : sorted_attrs) {
        std::string name_copy = name;
        AttrState* state = attrs_[name].get();
        futures.push_back(std::async(std::launch::async, [this, name_copy, state]() {
            finalize_attribute(name_copy, *state);
        }));
    }
    for (auto& f : futures)
        f.get();
    for (const auto& name : sorted_attrs) {
        std::cerr << "  " << name << " (" << attrs_[name]->id_to_str.size()
                  << " types) ...\n";
    }

    // Collect region_attrs for corpus.info (#8)
    std::vector<std::string> region_attrs_list;

    // Write non-sentence structural regions (and optional values)
    for (const auto& [type, regions] : regions_) {
        std::string base = output_dir_ + "/" + type;
        write_file(base + ".rgn", regions.data(), regions.size() * sizeof(Region));

        auto avit = region_attr_values_.find(type);
        if (avit != region_attr_values_.end() && !avit->second.empty()) {
            for (const auto& [attr_name, values] : avit->second) {
                if (values.size() != regions.size())
                    throw std::runtime_error("Region attr " + type + "_" + attr_name + " size mismatch");
                std::string attr_base = output_dir_ + "/" + type + "_" + attr_name;
                std::vector<int64_t> offsets;
                write_strings(attr_base + ".val", values, offsets);
                write_vec(attr_base + ".val.idx", offsets);
                region_attrs_list.push_back(type + "_" + attr_name);
            }
        } else {
            auto vit = region_values_.find(type);
            if (vit != region_values_.end() && !vit->second.empty()) {
                std::vector<int64_t> offsets;
                write_strings(base + ".val", vit->second, offsets);
                write_vec(base + ".val.idx", offsets);
            }
        }
    }

    // Write corpus.info
    {
        std::ofstream info(output_dir_ + "/corpus.info");
        if (!info) throw std::runtime_error("Cannot create corpus.info");
        info << "size=" << corpus_size_ << "\n";
        info << "positional=";
        for (size_t i = 0; i < sorted_attrs.size(); ++i) {
            if (i > 0) info << ",";
            info << sorted_attrs[i];
        }
        info << "\n";

        std::vector<std::string> structs;
        structs.push_back("s");
        for (const auto& s : struct_set_)
            if (s != "s") structs.push_back(s);
        std::sort(structs.begin(), structs.end());

        info << "structural=";
        for (size_t i = 0; i < structs.size(); ++i) {
            if (i > 0) info << ",";
            info << structs[i];
        }
        info << "\n";
        if (!region_attrs_list.empty()) {
            info << "region_attrs=";
            for (size_t i = 0; i < region_attrs_list.size(); ++i) {
                if (i > 0) info << ",";
                info << region_attrs_list[i];
            }
            info << "\n";
        }
        if (!default_within_.empty())
            info << "default_within=" << default_within_ << "\n";
    }

    finalized_ = true;
    std::cerr << "Done.\n";
}

// ── finalize_attribute: sort lexicon, remap .dat, build .rev ────────────

void StreamingBuilder::finalize_attribute(const std::string& name,
                                          AttrState& state) {
    std::string base = output_dir_ + "/" + name;
    int32_t lex_size = static_cast<int32_t>(state.id_to_str.size());

    // Choose optimal .dat width based on vocabulary size
    int dat_width = 4;
    if (lex_size < 256) dat_width = 1;
    else if (lex_size < 65536) dat_width = 2;

    // Choose optimal .rev width based on corpus size
    int rev_width = 8;
    if (corpus_size_ < 32768) rev_width = 2;
    else if (corpus_size_ < INT32_MAX) rev_width = 4;

    std::cerr << "    dat_width=" << dat_width << " rev_width=" << rev_width << "\n";

    // 1. Sort lexicon: compute permutation old_index → sorted position
    std::vector<int32_t> sorted_idx(lex_size);
    std::iota(sorted_idx.begin(), sorted_idx.end(), 0);
    std::sort(sorted_idx.begin(), sorted_idx.end(),
              [&](int32_t a, int32_t b) {
                  return state.id_to_str[a] < state.id_to_str[b];
              });

    // Build old→new remap: remap[old_id] = new_id
    std::vector<int32_t> remap(lex_size);
    for (int32_t new_id = 0; new_id < lex_size; ++new_id)
        remap[sorted_idx[new_id]] = new_id;

    // 2. Write .lex and .lex.idx (sorted strings)
    std::vector<std::string> sorted_strings(lex_size);
    for (int32_t i = 0; i < lex_size; ++i)
        sorted_strings[i] = state.id_to_str[sorted_idx[i]];

    std::vector<int64_t> lex_offsets;
    write_strings(base + ".lex", sorted_strings, lex_offsets);
    write_vec(base + ".lex.idx", lex_offsets);

    // Free lexicon memory — no longer needed
    state.str_to_id.clear();
    state.id_to_str.clear();
    sorted_strings.clear();
    sorted_strings.shrink_to_fit();

    // 3. Remap .dat (temp int32 IDs → sorted IDs, compacted to target width)
    remap_dat(base + ".dat", remap, dat_width);

    // 4. Build reverse index from the remapped .dat
    build_reverse_index(base, lex_size, rev_width);
}

// ── remap_dat: read temp IDs, apply remap, write final IDs ─────────────

void StreamingBuilder::remap_dat(const std::string& dat_path,
                                 const std::vector<int32_t>& remap,
                                 int target_width) {
    std::string tmp_path = dat_path + ".tmp";
    FILE* in  = fopen(dat_path.c_str(), "rb");
    FILE* out = fopen(tmp_path.c_str(), "wb");
    if (!in || !out) {
        if (in)  fclose(in);
        if (out) fclose(out);
        throw std::runtime_error("Cannot remap " + dat_path);
    }

    std::vector<int32_t> buf(IO_CHUNK);
    size_t nread;
    while ((nread = fread(buf.data(), sizeof(int32_t), IO_CHUNK, in)) > 0) {
        for (size_t i = 0; i < nread; ++i)
            buf[i] = remap[static_cast<size_t>(buf[i])];

        if (target_width == 1) {
            std::vector<uint8_t> narrow(nread);
            for (size_t i = 0; i < nread; ++i)
                narrow[i] = static_cast<uint8_t>(buf[i]);
            fwrite(narrow.data(), 1, nread, out);
        } else if (target_width == 2) {
            std::vector<uint16_t> narrow(nread);
            for (size_t i = 0; i < nread; ++i)
                narrow[i] = static_cast<uint16_t>(buf[i]);
            fwrite(narrow.data(), 2, nread, out);
        } else {
            fwrite(buf.data(), sizeof(int32_t), nread, out);
        }
    }

    fclose(in);
    fclose(out);
    fs::rename(tmp_path, dat_path);
}

// ── build_reverse_index: two-pass (count + fill via mmap) ──────────────

void StreamingBuilder::build_reverse_index(const std::string& base,
                                           int32_t lex_size,
                                           int rev_width) {
    std::string dat_path     = base + ".dat";
    std::string rev_path     = base + ".rev";
    std::string rev_idx_path = base + ".rev.idx";

    // Determine .dat element width so we can read the compacted file
    int dat_width = 4;
    {
        FILE* probe = fopen(dat_path.c_str(), "rb");
        if (!probe) throw std::runtime_error("Cannot open " + dat_path);
        fseek(probe, 0, SEEK_END);
        long fsize = ftell(probe);
        fclose(probe);
        if (corpus_size_ > 0)
            dat_width = static_cast<int>(static_cast<size_t>(fsize) /
                                         static_cast<size_t>(corpus_size_));
    }

    FILE* dat = fopen(dat_path.c_str(), "rb");
    if (!dat) throw std::runtime_error("Cannot open " + dat_path);

    // Pass 1: count positions per lex ID (reads dat with correct width)
    std::vector<int64_t> cnt(static_cast<size_t>(lex_size), 0);
    size_t nread;

    if (dat_width == 1) {
        std::vector<uint8_t> buf(IO_CHUNK);
        while ((nread = fread(buf.data(), 1, IO_CHUNK, dat)) > 0)
            for (size_t i = 0; i < nread; ++i) ++cnt[buf[i]];
    } else if (dat_width == 2) {
        std::vector<uint16_t> buf(IO_CHUNK);
        while ((nread = fread(buf.data(), 2, IO_CHUNK, dat)) > 0)
            for (size_t i = 0; i < nread; ++i) ++cnt[buf[i]];
    } else {
        std::vector<int32_t> buf(IO_CHUNK);
        while ((nread = fread(buf.data(), sizeof(int32_t), IO_CHUNK, dat)) > 0)
            for (size_t i = 0; i < nread; ++i)
                ++cnt[static_cast<size_t>(buf[i])];
    }

    // Compute prefix sums → rev.idx
    std::vector<int64_t> rev_idx(static_cast<size_t>(lex_size) + 1);
    rev_idx[0] = 0;
    for (int32_t i = 0; i < lex_size; ++i)
        rev_idx[static_cast<size_t>(i) + 1] =
            rev_idx[static_cast<size_t>(i)] + cnt[static_cast<size_t>(i)];

    write_vec(rev_idx_path, rev_idx);
    cnt.clear();

    int64_t total_pos = rev_idx[static_cast<size_t>(lex_size)];
    if (total_pos == 0) {
        fclose(dat);
        write_file(rev_path, nullptr, 0);
        return;
    }

    // Pass 2: fill .rev via writable mmap (at chosen rev_width)
    size_t rev_bytes = static_cast<size_t>(total_pos) * static_cast<size_t>(rev_width);
    int fd = ::open(rev_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::runtime_error("Cannot create " + rev_path);
    if (ftruncate(fd, static_cast<off_t>(rev_bytes)) != 0) {
        ::close(fd);
        throw std::runtime_error("Cannot set size of " + rev_path);
    }

    void* rev_raw = mmap(nullptr, rev_bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (rev_raw == MAP_FAILED) {
        ::close(fd);
        throw std::runtime_error("Cannot mmap " + rev_path + " for writing");
    }

    // Cursors: where to write the next position for each lex ID
    std::vector<int64_t> cursor(rev_idx.begin(),
                                rev_idx.begin() + lex_size);

    // Helper to write a position at cursor[id] with the right width
    auto write_pos = [&](size_t id, CorpusPos pos) {
        int64_t idx = cursor[id]++;
        switch (rev_width) {
            case 2:
                static_cast<int16_t*>(rev_raw)[idx] = static_cast<int16_t>(pos);
                break;
            case 4:
                static_cast<int32_t*>(rev_raw)[idx] = static_cast<int32_t>(pos);
                break;
            default:
                static_cast<int64_t*>(rev_raw)[idx] = static_cast<int64_t>(pos);
                break;
        }
    };

    rewind(dat);
    CorpusPos pos = 0;

    if (dat_width == 1) {
        std::vector<uint8_t> buf(IO_CHUNK);
        while ((nread = fread(buf.data(), 1, IO_CHUNK, dat)) > 0)
            for (size_t i = 0; i < nread; ++i) write_pos(buf[i], pos++);
    } else if (dat_width == 2) {
        std::vector<uint16_t> buf(IO_CHUNK);
        while ((nread = fread(buf.data(), 2, IO_CHUNK, dat)) > 0)
            for (size_t i = 0; i < nread; ++i) write_pos(buf[i], pos++);
    } else {
        std::vector<int32_t> buf(IO_CHUNK);
        while ((nread = fread(buf.data(), sizeof(int32_t), IO_CHUNK, dat)) > 0)
            for (size_t i = 0; i < nread; ++i)
                write_pos(static_cast<size_t>(buf[i]), pos++);
    }

    msync(rev_raw, rev_bytes, MS_SYNC);
    munmap(rev_raw, rev_bytes);
    ::close(fd);
    fclose(dat);
}

} // namespace manatree
