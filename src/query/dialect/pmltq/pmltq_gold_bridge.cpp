#include "query/dialect/pmltq/pmltq_gold_bridge.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace pando::pmltq {

namespace {

std::string shell_single_quote(const std::string& p) {
    std::string r = "'";
    for (char c : p) {
        if (c == '\'')
            r += "'\\''";
        else
            r += c;
    }
    r += '\'';
    return r;
}

} // namespace

bool pmltq_gold_run_node(const std::string& query, const std::string& js_dir,
                         const std::string& script_path, std::string* out_json, std::string* err_msg,
                         bool include_sql) {
    out_json->clear();
    err_msg->clear();

    if (js_dir.empty()) {
        *err_msg = "PMLTQ_GOLD_JS_DIR is empty";
        return false;
    }
    if (script_path.empty()) {
        *err_msg = "gold script path is empty";
        return false;
    }

    std::array<char, 64> tmpl{};
    std::memcpy(tmpl.data(), "/tmp/pando-pmltq-XXXXXX", 23);
    int fd = mkstemp(tmpl.data());
    if (fd < 0) {
        *err_msg = "mkstemp failed";
        return false;
    }
    {
        FILE* wf = fdopen(fd, "w");
        if (!wf) {
            close(fd);
            *err_msg = "fdopen failed";
            return false;
        }
        if (fwrite(query.data(), 1, query.size(), wf) != query.size()) {
            fclose(wf);
            unlink(tmpl.data());
            *err_msg = "write temp query failed";
            return false;
        }
        fclose(wf);
    }

    std::string cmd = "env PMLTQ_GOLD_JS_DIR=" + shell_single_quote(js_dir);
    if (include_sql)
        cmd += " PMLTQ_GOLD_INCLUDE_SQL=1";
    cmd += " node " + shell_single_quote(script_path) + " " + shell_single_quote(std::string(tmpl.data())) +
           " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        unlink(tmpl.data());
        *err_msg = "popen failed";
        return false;
    }

    std::ostringstream out;
    std::array<char, 8192> buf{};
    while (size_t n = fread(buf.data(), 1, buf.size(), pipe))
        out.write(buf.data(), static_cast<std::streamsize>(n));
    (void)pclose(pipe);
    unlink(tmpl.data());
    *out_json = out.str();

    if (out_json->empty() || (*out_json)[0] != '{') {
        *err_msg = "gold script did not return JSON object: " + *out_json;
        return false;
    }

    return true;
}

} // namespace pando::pmltq
