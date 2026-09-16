// ============================================================================
//  src/json.cpp — minimal read-only JSON parser (moved from main.cpp section 4)
// ============================================================================

#include "Chaos_Forger/json.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Chaos_Forger {

namespace {

class Parser {
public:
    Parser(const std::string& text) : text_(text) {}

    bool parse(Json& out, std::string& err) {
        err_.clear();
        skip_ws();
        if (!parse_value(out, 0)) {
            err = err_;
            return false;
        }
        skip_ws();
        if (pos_ != text_.size()) {
            err = "trailing characters after JSON value at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;
    std::string err_;
    static constexpr int kMaxDepth = 64;

    void fail(const std::string& msg) {
        if (err_.empty()) err_ = msg + " at offset " + std::to_string(pos_);
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool eat(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool parse_value(Json& out, int depth) {
        if (depth > kMaxDepth) {
            fail("JSON nesting too deep");
            return false;
        }
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return false;
        }
        switch (text_[pos_]) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                std::string s;
                if (!parse_string(s)) return false;
                out.value = std::move(s);
                return true;
            }
            case 't': return parse_literal("true", Json(true), out);
            case 'f': return parse_literal("false", Json(false), out);
            case 'n': return parse_literal("null", Json(), out);
            default:  return parse_number(out);
        }
    }

    bool parse_literal(const char* lit, Json&& result, Json& out) {
        const size_t len = std::strlen(lit);
        if (text_.compare(pos_, len, lit) != 0) {
            fail("invalid literal");
            return false;
        }
        pos_ += len;
        out.value = std::move(result.value);
        return true;
    }

    bool parse_number(Json& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool digits = false;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
            digits = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (!digits) {
            fail("invalid number");
            return false;
        }
        try {
            out.value = std::stod(text_.substr(start, pos_ - start));
        } catch (...) {
            fail("number out of range");
            return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (!eat('"')) {
            fail("expected string");
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) break;
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parse_hex4(cp)) return false;
                    // Surrogate pairs: combine high+low into one code point.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        const size_t save = pos_;
                        pos_ += 2;
                        unsigned low = 0;
                        if (parse_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = save;  // lone surrogate: keep as-is
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    fail("invalid escape sequence");
                    return false;
            }
        }
        fail("unterminated string");
        return false;
    }

    bool parse_hex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) {
            fail("truncated \\u escape");
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else {
                fail("invalid \\u escape");
                return false;
            }
        }
        return true;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_array(Json& out, int depth) {
        if (!eat('[')) {
            fail("expected '['");
            return false;
        }
        JsonArray arr;
        skip_ws();
        if (eat(']')) {
            out.value = std::move(arr);
            return true;
        }
        while (true) {
            skip_ws();
            Json element;
            if (!parse_value(element, depth + 1)) return false;
            arr.push_back(std::move(element));
            skip_ws();
            if (eat(',')) continue;
            if (eat(']')) {
                out.value = std::move(arr);
                return true;
            }
            fail("expected ',' or ']' in array");
            return false;
        }
    }

    bool parse_object(Json& out, int depth) {
        if (!eat('{')) {
            fail("expected '{'");
            return false;
        }
        JsonObject obj;
        skip_ws();
        if (eat('}')) {
            out.value = std::move(obj);
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (!eat(':')) {
                fail("expected ':' after object key");
                return false;
            }
            skip_ws();
            Json val;
            if (!parse_value(val, depth + 1)) return false;
            obj[std::move(key)] = std::move(val);
            skip_ws();
            if (eat(',')) continue;
            if (eat('}')) {
                out.value = std::move(obj);
                return true;
            }
            fail("expected ',' or '}' in object");
            return false;
        }
    }
};

}  // namespace

bool JsonParser::parse(const std::string& text, Json& out, std::string& err) {
    Parser impl(text);
    return impl.parse(out, err);
}

}  // namespace Chaos_Forger
