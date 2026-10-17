#pragma once

// Minimal JSON DOM for small, trusted data files (data/binary/*.json). UTF-8 in, UTF-8 strings out.
// Supports objects, arrays, strings (with \uXXXX escapes), numbers, booleans and null.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lie::json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value, std::less<>> obj;

    const Value* get(std::string_view key) const {
        if (kind != Kind::Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    bool is_string() const { return kind == Kind::String; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }
};

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool parse(Value& out, std::string* err) {
        skip_ws();
        if (!value(out)) return fail(err);
        skip_ws();
        if (i_ != s_.size()) {
            error_ = "trailing characters";
            return fail(err);
        }
        return true;
    }

private:
    bool fail(std::string* err) {
        if (err) *err = error_ + " at offset " + std::to_string(i_);
        return false;
    }
    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool lit(std::string_view w) {
        if (s_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }
    bool value(Value& v) {
        if (i_ >= s_.size()) return error("unexpected end");
        switch (s_[i_]) {
        case '{': return object(v);
        case '[': return array(v);
        case '"': v.kind = Value::Kind::String; return string(v.str);
        case 't': v.kind = Value::Kind::Bool; v.b = true; return lit("true") || error("bad literal");
        case 'f': v.kind = Value::Kind::Bool; v.b = false; return lit("false") || error("bad literal");
        case 'n': v.kind = Value::Kind::Null; return lit("null") || error("bad literal");
        default: return number(v);
        }
    }
    bool object(Value& v) {
        v.kind = Value::Kind::Object;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
        for (;;) {
            skip_ws();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"' || !string(key)) return error("expected key");
            skip_ws();
            if (i_ >= s_.size() || s_[i_++] != ':') return error("expected ':'");
            skip_ws();
            Value child;
            if (!value(child)) return false;
            v.obj.insert_or_assign(std::move(key), std::move(child));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
            return error("expected ',' or '}'");
        }
    }
    bool array(Value& v) {
        v.kind = Value::Kind::Array;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
        for (;;) {
            skip_ws();
            Value child;
            if (!value(child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
            return error("expected ',' or ']'");
        }
    }
    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(std::uint32_t& cp) {
        if (i_ + 4 > s_.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) break;
            switch (const char e = s_[i_++]) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) return error("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) {
                    std::uint32_t lo = 0;
                    if (!hex4(lo)) return error("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return error("bad escape");
            }
        }
        return error("unterminated string");
    }
    bool number(Value& v) {
        const std::size_t start = i_;
        while (i_ < s_.size() && s_[i_] != '\0' && std::strchr("+-.eE0123456789", s_[i_]) != nullptr) ++i_;
        if (start == i_) return error("unexpected character");
        const std::string tok(s_.substr(start, i_ - start));
        char* end = nullptr;
        v.kind = Value::Kind::Number;
        v.num = std::strtod(tok.c_str(), &end);
        return (end && *end == '\0') || error("bad number");
    }
    bool error(const char* what) {
        error_ = what;
        return false;
    }

    std::string_view s_;
    std::size_t i_ = 0;
    std::string error_;
};

inline bool parse(std::string_view text, Value& out, std::string* err = nullptr) {
    return Parser(text).parse(out, err);
}

} // namespace lie::json
