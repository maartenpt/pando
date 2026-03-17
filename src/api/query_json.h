#pragma once

#include "corpus/corpus.h"
#include "query/executor.h"
#include <string>
#include <vector>

namespace manatree {

struct QueryOptions {
    size_t limit   = 20;
    size_t offset  = 0;
    size_t max_total = 0;
    int context    = 5;
    bool total     = false;
    bool debug     = false;
    std::vector<std::string> attrs;  // empty = all token attributes in JSON; else only these
};

// Run a single query (one statement, no trailing command). Returns (MatchSet, elapsed_ms).
std::pair<MatchSet, double> run_single_query(const Corpus& corpus,
                                            const std::string& query_text,
                                            const QueryOptions& opts);

// Build JSON string for query result (same format as pando --json).
std::string to_query_result_json(const Corpus& corpus,
                                 const std::string& query_text,
                                 const MatchSet& ms,
                                 const QueryOptions& opts,
                                 double elapsed_ms);

// Build JSON string for corpus info (same format as pando --json with size on empty).
std::string to_info_json(const Corpus& corpus);

} // namespace manatree
