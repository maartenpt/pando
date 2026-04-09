#pragma once

#include "query/ast.h"
#include <string>

namespace pando {

// Translate PML-TQ into native pando Program. Semantic lowering will follow
// clickpmltq for token+region corpora; see dev/PMLTQ-ROADMAP.md.
// Throws std::runtime_error on parse errors or unsupported constructs.
Program translate_pmltq_program(const std::string& input, int debug_level,
                                std::string* trace_out);

// ClickPMLTQ reference SQL only (requires pmltq2sql-optimized.js next to the PEG parser).
// Pando does not execute this string; use for DB-backed mirrors of the same schema.
// Returns false and sets *err_msg on bridge/parse failure or translator error; *out_sql
// is the primary SELECT when successful.
bool translate_pmltq_export_click_sql(const std::string& input, std::string* out_sql,
                                      std::string* err_msg);

} // namespace pando
