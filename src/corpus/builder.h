#pragma once

#include "core/types.h"
#include "corpus/streaming_builder.h"
#include <string>
#include <memory>

namespace manatree {

// Convenience wrapper: reads CoNLL-U and feeds StreamingBuilder.
class CorpusBuilder {
public:
    explicit CorpusBuilder(const std::string& output_dir);

    void read_conllu(const std::string& path);
    /// Read CWB-style vertical: one token per line, tab-separated; <s> </s> for sentence boundaries.
    /// Columns: form [lemma [upos]]; no dependency info.
    void read_vertical(const std::string& path);

    /// Read JSONL event stream (from file or "-" = stdin) and feed StreamingBuilder.
    /// Events follow the schema in dev/PANDO-INDEX-INTEGRATION.md.
    void read_jsonl(const std::string& path);
    void finalize();

    // When true, split feats into individual feats_X attributes (old behavior).
    // When false (default), store feats as a single combined string.
    void set_split_feats(bool v) { split_feats_ = v; }

    StreamingBuilder& builder() { return builder_; }

private:
    void parse_feats(const std::string& feats_str,
                     std::unordered_map<std::string, std::string>& attrs);

    StreamingBuilder builder_;
    bool split_feats_ = false;

    // Region tracking for CoNLL-U input (3b/4c):
    // create text/doc and par regions with IDs from comment lines.
    bool        in_sentence_ = false;
    bool        has_doc_region_ = false;
    bool        has_par_region_ = false;
    CorpusPos   doc_start_ = 0;
    CorpusPos   par_start_ = 0;
    std::string doc_id_;
    std::string par_id_;

    // Region tracking for vertical/VRT input: stack of open regions
    // (<text ...>, <p ...>, <s ...>, <bla ...>), each with attributes.
    struct OpenRegion {
        std::string type;
        CorpusPos   start = 0;
        std::vector<std::pair<std::string, std::string>> attrs;
    };
    std::vector<OpenRegion> vrt_region_stack_;
};

} // namespace manatree
