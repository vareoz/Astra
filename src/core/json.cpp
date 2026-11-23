#include "core/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace astra {

namespace {

const std::string kEmptyString;
const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse_root(JsonValue& out, std::string* err) {
        skip_ws();
        if (!parse_value(out, err)) return false;
        skip_ws();
        if (pos_ != s_.size()) {
            if (err) *err = "Unexpected trailing characters in JSON";
            return false;
        }
        return true;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }

    bool parse_value(JsonValue& out, std::string* err) {
        skip_ws();
        if (pos_ >= s_.size()) {
            if (err) *err = "Unexpected end of JSON";
            return false;
        }
        char c = s_[pos_];
        if (c == '{') return parse_object(out, err);
        if (c == '[') return parse_array(out, err);
        if (c == '"') {
            std::string str;
            if (!parse_string(str, err)) return false;
            out = JsonValue(std::move(str));
            return true;
        }
        if (c == 't' && s_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out = JsonValue(true);
            return true;
        }
        if (c == 'f' && s_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out = JsonValue(false);
            return true;
        }
        if (c == 'n' && s_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out = JsonValue(nullptr);
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number(out, err);
        }
        if (err) *err = std::string("Unexpected token '") + c + "'";
        return false;
    }

    bool parse_string(std::string& out, std::string* err) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) {
                    if (err) *err = "Unterminated escape";
                    return false;
                }
                char esc = s_[pos_++];
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
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned code = std::strtoul(std::string(s_.substr(pos_, 4)).c_str(), nullptr, 16);
                        pos_ += 4;
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        if (err) *err = "Unterminated string";
        return false;
    }

    bool parse_number(JsonValue& out, std::string* err) {
        size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        std::string token(s_.substr(start, pos_ - start));
        char* endptr = nullptr;
        double val = std::strtod(token.c_str(), &endptr);
        if (!endptr || *endptr != '\0') {
            if (err) *err = "Invalid number: " + token;
            return false;
        }
        out = JsonValue(val);
        return true;
    }

    bool parse_array(JsonValue& out, std::string* err) {
        ++pos_; // '['
        JsonValue::Array arr;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            out = JsonValue(std::move(arr));
            return true;
        }
        while (pos_ < s_.size()) {
            JsonValue elem;
            if (!parse_value(elem, err)) return false;
            arr.push_back(std::move(elem));
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                out = JsonValue(std::move(arr));
                return true;
            }
            if (err) *err = "Expected ',' or ']' in array";
            return false;
        }
        if (err) *err = "Unterminated array";
        return false;
    }

    bool parse_object(JsonValue& out, std::string* err) {
        ++pos_; // '{'
        JsonValue::Object obj;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            out = JsonValue(std::move(obj));
            return true;
        }
        while (pos_ < s_.size()) {
            skip_ws();
            std::string key;
            if (!parse_string(key, err)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                if (err) *err = "Expected ':' after object key";
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!parse_value(val, err)) return false;
            obj[std::move(key)] = std::move(val);
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                out = JsonValue(std::move(obj));
                return true;
            }
            if (err) *err = "Expected ',' or '}' in object";
            return false;
        }
        if (err) *err = "Unterminated object";
        return false;
    }

    std::string_view s_;
    size_t pos_ = 0;
};

void escape_json_string(const std::string& s, std::ostringstream& oss) {
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c; break;
        }
    }
    oss << '"';
}

} // namespace

JsonValue::Type JsonValue::type() const {
    return static_cast<Type>(data_.index());
}

bool JsonValue::as_bool(bool def) const {
    if (auto* p = std::get_if<bool>(&data_)) return *p;
    return def;
}

double JsonValue::as_number(double def) const {
    if (auto* p = std::get_if<double>(&data_)) return *p;
    return def;
}

const std::string& JsonValue::as_string() const {
    if (auto* p = std::get_if<std::string>(&data_)) return *p;
    return kEmptyString;
}

const JsonValue::Array& JsonValue::as_array() const {
    if (auto* p = std::get_if<Array>(&data_)) return *p;
    return kEmptyArray;
}

const JsonValue::Object& JsonValue::as_object() const {
    if (auto* p = std::get_if<Object>(&data_)) return *p;
    return kEmptyObject;
}

JsonValue::Object& JsonValue::as_object_mut() {
    if (!is_object()) data_ = Object{};
    return std::get<Object>(data_);
}

JsonValue::Array& JsonValue::as_array_mut() {
    if (!is_array()) data_ = Array{};
    return std::get<Array>(data_);
}

const JsonValue* JsonValue::get(const std::string& key) const {
    if (!is_object()) return nullptr;
    const auto& obj = std::get<Object>(data_);
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
}

std::string JsonValue::get_string(const std::string& key, const std::string& def) const {
    const JsonValue* v = get(key);
    return (v && v->is_string()) ? v->as_string() : def;
}

bool JsonValue::get_bool(const std::string& key, bool def) const {
    const JsonValue* v = get(key);
    return (v && v->is_bool()) ? v->as_bool(def) : def;
}

double JsonValue::get_number(const std::string& key, double def) const {
    const JsonValue* v = get(key);
    return (v && v->is_number()) ? v->as_number(def) : def;
}

std::string JsonValue::dump() const {
    std::ostringstream oss;
    switch (type()) {
        case Type::Null: oss << "null"; break;
        case Type::Bool: oss << (as_bool() ? "true" : "false"); break;
        case Type::Number: {
            double n = as_number();
            if (std::floor(n) == n && std::abs(n) < 1e15) {
                oss << static_cast<int64_t>(n);
            } else {
                oss << n;
            }
            break;
        }
        case Type::String: escape_json_string(as_string(), oss); break;
        case Type::Array: {
            oss << '[';
            const auto& arr = as_array();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) oss << ',';
                oss << arr[i].dump();
            }
            oss << ']';
            break;
        }
        case Type::Object: {
            oss << '{';
            const auto& obj = as_object();
            bool first = true;
            for (const auto& [k, v] : obj) {
                if (!first) oss << ',';
                first = false;
                escape_json_string(k, oss);
                oss << ':' << v.dump();
            }
            oss << '}';
            break;
        }
    }
    return oss.str();
}

bool JsonValue::parse(std::string_view input, JsonValue& out, std::string* error) {
    Parser p(input);
    return p.parse_root(out, error);
}

} // namespace astra
