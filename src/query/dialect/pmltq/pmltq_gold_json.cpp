#include "query/dialect/pmltq/pmltq_gold_json.h"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace manatree::pmltq {

namespace {

void skip_ws(std::string_view& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
}

bool parse_value(std::string_view& s, JsonValue& out, std::string& err);

bool parse_string(std::string_view& s, std::string& out, std::string& err) {
    if (s.empty() || s[0] != '"') {
        err = "expected '\"'";
        return false;
    }
    s.remove_prefix(1);
    std::string r;
    while (!s.empty()) {
        char c = s[0];
        s.remove_prefix(1);
        if (c == '"') {
            out = std::move(r);
            return true;
        }
        if (c == '\\' && !s.empty()) {
            char e = s[0];
            s.remove_prefix(1);
            switch (e) {
            case '"':
            case '\\':
            case '/':
                r += e;
                break;
            case 'b':
                r += '\b';
                break;
            case 'f':
                r += '\f';
                break;
            case 'n':
                r += '\n';
                break;
            case 'r':
                r += '\r';
                break;
            case 't':
                r += '\t';
                break;
            case 'u':
                if (s.size() < 4) {
                    err = "truncated \\u escape";
                    return false;
                }
                // Skip basic unicode escapes (optional full implementation)
                r += '?';
                s.remove_prefix(4);
                break;
            default:
                r += e;
                break;
            }
            continue;
        }
        r += c;
    }
    err = "unterminated string";
    return false;
}

bool parse_number(std::string_view& s, double& out) {
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' ||
                            s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
        ++i;
    if (i == 0)
        return false;
    std::string chunk(s.substr(0, i));
    s.remove_prefix(i);
    try {
        out = std::stod(chunk);
    } catch (...) {
        return false;
    }
    return true;
}

bool parse_array(std::string_view& s, JsonValue& out, std::string& err) {
    if (s.empty() || s[0] != '[') {
        err = "expected '['";
        return false;
    }
    s.remove_prefix(1);
    skip_ws(s);
    out.kind = JsonValue::Array;
    if (!s.empty() && s[0] == ']') {
        s.remove_prefix(1);
        return true;
    }
    for (;;) {
        JsonValue el;
        if (!parse_value(s, el, err))
            return false;
        out.arr_val.push_back(std::move(el));
        skip_ws(s);
        if (s.empty()) {
            err = "unterminated array";
            return false;
        }
        if (s[0] == ']') {
            s.remove_prefix(1);
            return true;
        }
        if (s[0] != ',') {
            err = "expected ',' or ']' in array";
            return false;
        }
        s.remove_prefix(1);
        skip_ws(s);
    }
}

bool parse_object(std::string_view& s, JsonValue& out, std::string& err) {
    if (s.empty() || s[0] != '{') {
        err = "expected '{'";
        return false;
    }
    s.remove_prefix(1);
    skip_ws(s);
    out.kind = JsonValue::Object;
    if (!s.empty() && s[0] == '}') {
        s.remove_prefix(1);
        return true;
    }
    for (;;) {
        skip_ws(s);
        std::string key;
        if (!parse_string(s, key, err))
            return false;
        skip_ws(s);
        if (s.empty() || s[0] != ':') {
            err = "expected ':' after object key";
            return false;
        }
        s.remove_prefix(1);
        skip_ws(s);
        JsonValue val;
        if (!parse_value(s, val, err))
            return false;
        out.obj_val.push_back({std::move(key), std::move(val)});
        skip_ws(s);
        if (s.empty()) {
            err = "unterminated object";
            return false;
        }
        if (s[0] == '}') {
            s.remove_prefix(1);
            return true;
        }
        if (s[0] != ',') {
            err = "expected ',' or '}' in object";
            return false;
        }
        s.remove_prefix(1);
    }
}

bool parse_value(std::string_view& s, JsonValue& out, std::string& err) {
    skip_ws(s);
    if (s.empty()) {
        err = "unexpected end of input";
        return false;
    }
    if (s[0] == '"') {
        out.kind = JsonValue::String;
        return parse_string(s, out.s_val, err);
    }
    if (s[0] == '[')
        return parse_array(s, out, err);
    if (s[0] == '{')
        return parse_object(s, out, err);
    if (s.size() >= 4 && s.substr(0, 4) == "null") {
        s.remove_prefix(4);
        out.kind = JsonValue::Null;
        return true;
    }
    if (s.size() >= 4 && s.substr(0, 4) == "true") {
        s.remove_prefix(4);
        out.kind = JsonValue::Bool;
        out.b_val = true;
        return true;
    }
    if (s.size() >= 5 && s.substr(0, 5) == "false") {
        s.remove_prefix(5);
        out.kind = JsonValue::Bool;
        out.b_val = false;
        return true;
    }
    double n;
    if (parse_number(s, n)) {
        out.kind = JsonValue::Number;
        out.n_val = n;
        return true;
    }
    err = "invalid JSON token";
    return false;
}

} // namespace

const JsonValue* JsonValue::get(std::string_view key) const {
    for (const auto& p : obj_val) {
        if (p.first == key)
            return &p.second;
    }
    return nullptr;
}

JsonValue* JsonValue::get_mut(std::string_view key) {
    for (auto& p : obj_val) {
        if (p.first == key)
            return &p.second;
    }
    return nullptr;
}

std::string JsonValue::as_string() const {
    if (kind == String)
        return s_val;
    if (kind == Number) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", n_val);
        return std::string(buf);
    }
    if (kind == Bool)
        return b_val ? "true" : "false";
    return {};
}

bool JsonValue::as_bool(bool def) const {
    if (kind == Bool)
        return b_val;
    return def;
}

bool parse_json(std::string_view in, JsonValue& out, std::string& err) {
    if (!parse_value(in, out, err))
        return false;
    skip_ws(in);
    if (!in.empty()) {
        err = "trailing garbage after JSON";
        return false;
    }
    return true;
}

} // namespace manatree::pmltq
