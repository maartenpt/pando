#include "query/dialect/pmltq/pmltq_translate.h"

#include "query/dialect/pmltq/pmltq_gold_bridge.h"
#include "query/dialect/pmltq/pmltq_gold_json.h"
#include "query/dialect/pmltq/pmltq_gold_lower.h"
#include "query/dialect/pmltq/pmltq_gold_paths.h"
#include "query/dialect/pmltq/pmltq_native_lower.h"
#include "query/dialect/pmltq/pmltq_parser.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace pando {

// PML-TQ: JSON ast → lower_gold_ast_to_program() → native Program → QueryExecutor on
// the loaded Corpus. Optional Node only supplies a reference PEG parse until the C++
// parser exists; pando never executes ClickHouse/SQL.

Program translate_pmltq_program(const std::string& input, int debug_level,
                                std::string* trace_out) {
    if (trace_out)
        trace_out->clear();

    std::string native_err;
    try {
        pmltq::PmltqQuery q = pmltq::parse_pmltq_query(input);
        std::string err_lo;
        Program p = pmltq::lower_native_pmltq_query(q, err_lo);
        if (!err_lo.empty())
            throw std::runtime_error(err_lo);
        if (debug_level > 0 && trace_out)
            *trace_out += "PML-TQ: native parser + lowerer.\n";
        return p;
    } catch (const std::exception& e) {
        native_err = e.what();
    }

    const char* js_dir = std::getenv("PMLTQ_GOLD_JS_DIR");
    if (!js_dir || js_dir[0] == '\0') {
        throw std::runtime_error(native_err);
    }

    const char* script_env = std::getenv("PMLTQ_GOLD_SCRIPT");
    std::string script = (script_env && script_env[0]) ? std::string(script_env)
                                                       : std::string(pmltq::pmltq_gold_script_path_default());

    std::string json_raw;
    std::string err_run;
    if (!pmltq::pmltq_gold_run_node(input, js_dir, script, &json_raw, &err_run))
        throw std::runtime_error("PMLTQ gold bridge: " + err_run + " (native error: " + native_err + ")");

    pmltq::JsonValue root;
    std::string err_parse;
    if (!pmltq::parse_json(json_raw, root, err_parse))
        throw std::runtime_error("PMLTQ gold JSON: " + err_parse + " (native error: " + native_err + ")");

    const pmltq::JsonValue* ok = root.get("ok");
    if (ok && ok->kind == pmltq::JsonValue::Bool && !ok->b_val) {
        const pmltq::JsonValue* em = root.get("error");
        throw std::runtime_error((em ? em->as_string() : "PMLTQ gold parse failed") +
                                 std::string(" (native error: ") + native_err + ")");
    }

    if (debug_level > 0 && trace_out) {
        *trace_out +=
            "PML-TQ: gold AST → native lowerer (native parse failed: " + native_err + ").\n";
    }

    std::string err_lo;
    Program p = pmltq::lower_gold_ast_to_program(root, err_lo);
    if (!err_lo.empty())
        throw std::runtime_error(err_lo + " (native error: " + native_err + ")");
    return p;
}

bool translate_pmltq_export_click_sql(const std::string& input, std::string* out_sql,
                                      std::string* err_msg) {
    out_sql->clear();
    err_msg->clear();

    const char* js_dir = std::getenv("PMLTQ_GOLD_JS_DIR");
    if (!js_dir || js_dir[0] == '\0') {
        *err_msg = "PMLTQ_GOLD_JS_DIR must be set (directory with pmltq-parser-umd.js and "
                   "pmltq2sql-optimized.js for SQL export)";
        return false;
    }

    const char* script_env = std::getenv("PMLTQ_GOLD_SCRIPT");
    std::string script = (script_env && script_env[0]) ? std::string(script_env)
                                                       : std::string(pmltq::pmltq_gold_script_path_default());

    std::string json_raw;
    std::string err_run;
    if (!pmltq::pmltq_gold_run_node(input, js_dir, script, &json_raw, &err_run, true)) {
        *err_msg = err_run;
        return false;
    }

    pmltq::JsonValue root;
    std::string err_parse;
    if (!pmltq::parse_json(json_raw, root, err_parse)) {
        *err_msg = "PMLTQ gold JSON: " + err_parse;
        return false;
    }

    const pmltq::JsonValue* ok = root.get("ok");
    if (ok && ok->kind == pmltq::JsonValue::Bool && !ok->b_val) {
        const pmltq::JsonValue* em = root.get("error");
        *err_msg = em ? em->as_string() : "PMLTQ gold parse failed";
        return false;
    }

    const pmltq::JsonValue* sqlv = root.get("sql");
    if (!sqlv || sqlv->kind == pmltq::JsonValue::Null) {
        *err_msg = "gold script returned no sql field (update dev/pmltq-gold/parse_ast_sql.js; "
                   "need pmltq2sql-optimized.js in PMLTQ_GOLD_JS_DIR)";
        return false;
    }

    if (sqlv->kind == pmltq::JsonValue::Object) {
        const pmltq::JsonValue* er = sqlv->get("error");
        if (er && er->kind != pmltq::JsonValue::Null) {
            std::string es = er->as_string();
            if (!es.empty()) {
                *err_msg = std::move(es);
                return false;
            }
        }
        const pmltq::JsonValue* sq = sqlv->get("sql");
        if (sq && sq->kind == pmltq::JsonValue::String && !sq->s_val.empty()) {
            *out_sql = sq->s_val;
            return true;
        }
    }

    *err_msg = "gold SQL object had no usable sql string";
    return false;
}

} // namespace pando
