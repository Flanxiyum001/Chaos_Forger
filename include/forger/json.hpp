#pragma once
// ============================================================================
//  forger/json.hpp — minimal read-only JSON value + recursive-descent parser.
//  Handles nested objects/arrays, escapes, surrogate pairs, depth limits,
//  and rejects trailing garbage. No serialization, no third-party code.
// ============================================================================

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace forger {

struct Json;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;

struct Json {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> value;

    Json() : value(nullptr) {}
    explicit Json(bool b) : value(b) {}
    explicit Json(double d) : value(d) {}
    explicit Json(const char* s) : value(std::string(s)) {}
    explicit Json(std::string s) : value(std::move(s)) {}
    explicit Json(JsonArray a) : value(std::move(a)) {}
    explicit Json(JsonObject o) : value(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value); }
    bool is_bool() const { return std::holds_alternative<bool>(value); }
    bool is_number() const { return std::holds_alternative<double>(value); }
    bool is_string() const { return std::holds_alternative<std::string>(value); }
    bool is_array() const { return std::holds_alternative<JsonArray>(value); }
    bool is_object() const { return std::holds_alternative<JsonObject>(value); }

    const std::string& as_string() const { return std::get<std::string>(value); }
    double as_number() const { return std::get<double>(value); }
    bool as_bool() const { return std::get<bool>(value); }
    const JsonArray& as_array() const { return std::get<JsonArray>(value); }
    const JsonObject& as_object() const { return std::get<JsonObject>(value); }

    const Json* find(const std::string& key) const {
        if (!is_object()) return nullptr;
        const auto& obj = as_object();
        const auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    // Parses a complete document; fails on trailing garbage. `err` is set on
    // failure and left empty on success.
    static bool parse(const std::string& text, Json& out, std::string& err);
};

}  // namespace forger
