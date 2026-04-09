#pragma once

#include "query/executor.h"

namespace pando {

bool build_aggregate_plan(const Corpus& corpus, const std::vector<std::string>& fields,
                          AggregateBucketData& out);

bool fill_aggregate_key(AggregateBucketData& data, const Corpus& corpus, const Match& m,
                        const NameIndexMap& nm, std::vector<int64_t>& key_out);

} // namespace pando
