#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace astra {

class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : data_(std::monostate{}) {}
    JsonValue(std::nullptr_t) : data_(std::monostate{}) {}
    JsonValue(bool b) : data_(b) {}
    JsonValue(double n) : data_(n) {}
    JsonValue(int n) : data_(static_cast<double>(n)) {}
    JsonValue(std::string s) : data_(std::move(s)) {}
    JsonValue(const char* s) : data_(std::string(s)) {}
    JsonValue(Array a) : data_(std::move(a)) {}
    JsonValue(Object o) : data_(std::move(o)) {}

    Type type() const;
    bool is_null() const { return type() == Type::Null; }
    bool is_bool() const { return type() == Type::Bool; }
    bool is_number() const { return type() == Type::Number; }
    bool is_string() const { return type() == Type::String; }
    bool is_array() const { return type() == Type::Array; }
    bool is_object() const { return type() == Type::Object; }

    bool as_bool(bool def = false) const;
    double as_number(double def = 0.0) const;
    int64_t as_int(int64_t def = 0) const { return static_cast<int64_t>(as_number(static_cast<double>(def))); }
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;
    Object& as_object_mut();
    Array& as_array_mut();

    const JsonValue* get(const std::string& key) const;
    std::string get_string(const std::string& key, const std::string& def = "") const;
    bool get_bool(const std::string& key, bool def = false) const;
    double get_number(const std::string& key, double def = 0.0) const;

    std::string dump() const;
    static bool parse(std::string_view input, JsonValue& out, std::string* error = nullptr);

private:
    std::variant<std::monostate, bool, double, std::string, Array, Object> data_;
};

} // namespace astra
