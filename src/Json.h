#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace esb {
namespace json {

// A deliberately small JSON implementation. The bridge needs exactly two
// things -- a state file it wrote itself, and a config file a user edits by
// hand -- and taking a dependency for that would cost more than it saves:
// the whole point of the DLL is that it ships with no redistributable and no
// package manager (README, "Dependencies: none").
//
// Objects keep their keys sorted, so a written state file diffs cleanly.
class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    explicit Value(bool b)               : m_type(Type::Bool),   m_bool(b) {}
    explicit Value(double n)             : m_type(Type::Number), m_number(n) {}
    explicit Value(int64_t n)            : m_type(Type::Number), m_number(static_cast<double>(n)) {}
    explicit Value(const std::string& s) : m_type(Type::String), m_string(s) {}
    explicit Value(const char* s)        : m_type(Type::String), m_string(s ? s : "") {}

    static Value Object() { Value v; v.m_type = Type::Object; return v; }
    static Value Array()  { Value v; v.m_type = Type::Array;  return v; }

    Type type() const { return m_type; }
    bool IsNull()   const { return m_type == Type::Null; }
    bool IsBool()   const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray()  const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }

    bool               AsBool(bool fallback = false) const;
    double             AsNumber(double fallback = 0.0) const;
    int64_t            AsInt(int64_t fallback = 0) const;
    const std::string& AsString() const;

    // Object access. Missing keys give a null Value rather than throwing --
    // a hand-edited config is allowed to be incomplete.
    const Value& operator[](const std::string& key) const;
    bool         Has(const std::string& key) const;
    void         Set(const std::string& key, const Value& v);
    const std::map<std::string, Value>& Members() const { return m_object; }

    // Array access.
    void                      Push(const Value& v) { m_array.push_back(v); }
    size_t                    Size() const { return m_array.size(); }
    const std::vector<Value>& Items() const { return m_array; }

private:
    Type                        m_type   = Type::Null;
    bool                        m_bool   = false;
    double                      m_number = 0.0;
    std::string                 m_string;
    std::vector<Value>          m_array;
    std::map<std::string, Value> m_object;
};

// Returns a Null value and sets *error on malformed input. Never throws:
// a corrupt state file must degrade to "no saved state", not take the
// plugin down at load.
Value Parse(const std::string& text, std::string* error = nullptr);

std::string Write(const Value& v, bool pretty = true);

} // namespace json
} // namespace esb
