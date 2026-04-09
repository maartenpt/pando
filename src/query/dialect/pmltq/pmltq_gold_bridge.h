#pragma once

#include <string>

namespace pando::pmltq {

struct JsonValue;

// Run dev/pmltq-gold/parse_ast_sql.js via Node. PMLTQ_GOLD_JS_DIR = dir with
// pmltq-parser-umd.js. If include_sql, also sets PMLTQ_GOLD_INCLUDE_SQL=1 so the
// script may load pmltq2sql-optimized.js (ClickPMLTQ reference SQL, not executed by pando).
// On success, *out_json is `{ ok, ast }` or `{ ok, ast, sql }`.
bool pmltq_gold_run_node(const std::string& query, const std::string& js_dir,
                         const std::string& script_path, std::string* out_json, std::string* err_msg,
                         bool include_sql = false);

} // namespace pando::pmltq
