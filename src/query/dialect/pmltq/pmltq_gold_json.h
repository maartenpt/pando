#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manatree::pmltq {

// Minimal JSON value for gold AST / bridge responses (no third-party dependency).
struct JsonValue {
    enum Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Null;
    bool b_val = false;
    double n_val = 0;
    std::string s_val;
    std::vector<JsonValue> arr_val;
    /// Object members (order preserved for debugging).
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    const JsonValue* get(std::string_view key) const;
    JsonValue* get_mut(std::string_view key);

    std::string as_string() const;
    bool as_bool(bool def = false) const;
};

bool parse_json(std::string_view in, JsonValue& out, std::string& err);

} // namespace manatree::pmltq
